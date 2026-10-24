// Exact fade tables of the cockpit band (`M8.5` slice 3): render/hud_lut.h.
//
// Three halves:
//
//  * synthetic - the 4096-byte contract (a missing file, an empty file, 4095
//    and 4097 bytes are refused with text; a good file loads, `stats` carries
//    source/bytes/shape, the tables are positional, `map` clamps to 0..15 and
//    99 == 15), `describe` is 16 lines, `build_model_lut` is the documented
//    `(v*k + max/2)/max` model, and `build_hud_lut` refuses a palette that is
//    not 1024 bytes / has no data;
//  * built - `build_hud_lut` from the installed `FRONTEND/GAMEPAL`: it
//    reproduces the dump byte for byte (4096/4096, FNV 0x6330406254771f3d),
//    and without the dump the pinned properties still hold (level 11 colour
//    identity 256/256, level 0 = 236 black + 20 untouched, 12..15 =
//    132/73/48/45 colour identities);
//  * real `~/.cache/gunmetal-re/hud_lut.bin` - the numbers of
//    docs/HUD_LUT.md §2: 16 tables, level 11 identity (254/256 by index,
//    256/256 by colour), level 0 black except the 20 slots outside the palette
//    ramps, levels 12..15 = 132/73/48/45 colour identities (the fact that
//    contradicts "near-identity"), and the per-level agreement of the RGBA
//    model with the dump.  Missing dump or palette prints SKIP and the test
//    still passes.
//
// Every check prints its numbers: a green run is the record the doc quotes.
#include "render/hud_lut.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "res/bmp.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__,  \
                         __LINE__, #a, #b, static_cast<long long>(va),               \
                         static_cast<long long>(vb));                                \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

int count_lines(const std::string& text) {
    int lines = text.empty() ? 0 : 1;
    for (const char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    return lines;
}

bool write_bytes(const fs::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(out);
}

/// The dump: `GM_HUD_LUT`, else `~/.cache/gunmetal-re/hud_lut.bin`
/// (docs/HUD_AUDIT.md §2.8: `tools/win/dump_lut.lua` writes it there).
fs::path find_dump() {
    if (const char* env = std::getenv("GM_HUD_LUT"); env != nullptr && *env != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".cache" / "gunmetal-re" / "hud_lut.bin";
    }
    return {};
}

/// The installation for `GAMEPAL`: `GUNMETAL_DIR`, the reference install, the
/// mirror.  Any root is returned; the palette loader decides whether it works.
fs::path find_game() {
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    std::error_code ec;
    for (const fs::path& root : roots) {
        if (fs::is_directory(root / "FRONTEND", ec) && !ec) {
            return root;
        }
    }
    return {};
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb palette_rgb(const std::vector<std::uint8_t>& palette, int index) {
    const std::size_t at = static_cast<std::size_t>(index) * 4u;
    return Rgb{palette[at], palette[at + 1], palette[at + 2]};
}

// ---------------------------------------------------------------- synthetic

void test_synthetic() {
    const fs::path root = fs::temp_directory_path() / "gm_hud_lut_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    gm::HudLut lut;
    gm::HudLutStats stats;
    std::string error;

    // Missing file: refused with text, nothing half-loaded.
    {
        const fs::path missing = root / "missing.bin";
        CHECK(!gm::load_hud_lut(missing, lut, &error, &stats));
        CHECK(contains(error, "hud_lut"));
        CHECK(contains(error, "cannot read"));
        CHECK_EQ(stats.bytes, std::size_t{0});
        CHECK_EQ(stats.levels, 0);
        std::printf("synthetic missing: %s\n", error.c_str());
        for (const auto& table : lut.levels) {
            for (const std::uint8_t entry : table) {
                CHECK_EQ(entry, 0);
            }
        }
    }

    // Empty, short and long files: the size is the whole contract.
    constexpr std::size_t kSizes[2] = {gm::kHudLutBytes - 1, gm::kHudLutBytes + 1};
    for (const std::size_t size : kSizes) {
        const fs::path file = root / ("bad_" + std::to_string(size) + ".bin");
        CHECK(write_bytes(file, std::vector<std::uint8_t>(size, 0xAB)));
        CHECK(!gm::load_hud_lut(file, lut, &error, &stats));
        CHECK(contains(error, std::to_string(size)));
        CHECK(contains(error, "expected 4096"));
        std::printf("synthetic %zu bytes: %s\n", size, error.c_str());
    }
    {
        const fs::path empty = root / "empty.bin";
        CHECK(write_bytes(empty, {}));
        CHECK(!gm::load_hud_lut(empty, lut, &error, &stats));
        CHECK(contains(error, " 0 bytes"));
        std::printf("synthetic empty: %s\n", error.c_str());
    }

    // A good file: levels[k][i] = (i + k) & 0xFF, so every level is a known
    // shift and the positional layout is visible in `map`.
    std::vector<std::uint8_t> bytes(gm::kHudLutBytes);
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        for (int index = 0; index < gm::kHudLutEntries; ++index) {
            bytes[static_cast<std::size_t>(level) * gm::kHudLutEntries +
                  static_cast<std::size_t>(index)] =
                static_cast<std::uint8_t>((index + level) & 0xFF);
        }
    }
    const fs::path good = root / "good.bin";
    CHECK(write_bytes(good, bytes));
    stats = gm::HudLutStats{};
    CHECK(gm::load_hud_lut(good, lut, &error, &stats));
    CHECK(error.empty());
    CHECK(stats.source == good.string());
    CHECK_EQ(stats.bytes, gm::kHudLutBytes);
    CHECK_EQ(stats.levels, gm::kHudLutLevels);
    CHECK_EQ(stats.entries, gm::kHudLutEntries);
    std::printf("synthetic good: source=%s bytes=%zu levels=%d entries=%d\n",
                stats.source.c_str(), stats.bytes, stats.levels, stats.entries);

    // Positional: level k is the file bytes k*256..k*256+255, not a rotation.
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        for (int index = 0; index < gm::kHudLutEntries; ++index) {
            const auto expected =
                static_cast<std::uint8_t>((index + level) & 0xFF);
            CHECK_EQ(gm::map(lut, level, static_cast<std::uint8_t>(index)), expected);
        }
    }
    // Clamp: 99 and 15 are the same table, -7 and 0 are the same table, and
    // 11 is a real table (not an alias of 15).
    for (int index = 0; index < gm::kHudLutEntries; ++index) {
        const auto i = static_cast<std::uint8_t>(index);
        CHECK_EQ(gm::map(lut, 99, i), gm::map(lut, gm::kHudLutMaxLevel, i));
        CHECK_EQ(gm::map(lut, -7, i), gm::map(lut, 0, i));
    }
    CHECK_EQ(gm::map(lut, 15, 0), static_cast<std::uint8_t>(0 + 15));
    CHECK_EQ(gm::map(lut, 99, 0), static_cast<std::uint8_t>(15));
    std::printf("synthetic map: level 0 -> %d, 11 -> %d, 15 -> %d, 99 -> %d (index 0)\n",
                gm::map(lut, 0, 0), gm::map(lut, 11, 0), gm::map(lut, 15, 0),
                gm::map(lut, 99, 0));

    // describe: 16 lines, no palette means "not measured", not a zero.
    const std::string no_palette = gm::describe(lut);
    CHECK_EQ(count_lines(no_palette), gm::kHudLutLevels);
    CHECK(contains(no_palette, "colour-identity n/a"));
    CHECK(!contains(no_palette, "colour-identity   0"));

    // The shift table: level 0 is the identity, whites travel with it.
    const gm::HudLutLevel shift0 = gm::level_report(lut, 0);
    CHECK_EQ(shift0.identity, 256);
    CHECK_EQ(shift0.colour_identity, -1);
    CHECK_EQ(shift0.white, 1);
    CHECK_EQ(shift0.min, 0);
    CHECK_EQ(shift0.max, 255);
    const gm::HudLutLevel shift1 = gm::level_report(lut, 1);
    CHECK_EQ(shift1.identity, 0);
    CHECK_EQ(shift1.white, 1);
    std::printf("synthetic describe: level 0 identity=%d white=%d min=%d max=%d, "
                "level 1 identity=%d\n",
                shift0.identity, shift0.white, shift0.min, shift0.max, shift1.identity);

    // A palette of the wrong size is "not measured", never a wrong count.
    std::vector<std::uint8_t> short_palette(1020, 0);
    CHECK_EQ(gm::level_report(lut, 0, short_palette).colour_identity, -1);

    // Determinism: two loads of the same bytes give the same tables and text.
    gm::HudLut again;
    CHECK(gm::load_hud_lut(good, again, &error));
    CHECK(lut == again);
    CHECK(gm::describe(lut, {}) == gm::describe(again, {}));
    CHECK_EQ(fnv1a64(reinterpret_cast<const std::uint8_t*>(&lut),
                     sizeof(lut)),
             fnv1a64(reinterpret_cast<const std::uint8_t*>(&again), sizeof(again)));
    std::printf("synthetic determinism: tables equal, FNV-1a64 0x%016llx\n",
                static_cast<unsigned long long>(
                    fnv1a64(reinterpret_cast<const std::uint8_t*>(&lut), sizeof(lut))));

    // build_model_lut: the documented (v*k + max/2)/max, clamped at max.
    {
        gm::HudLut model;
        CHECK(!gm::build_model_lut(model, 0, &error));
        CHECK(contains(error, "outside 1..15"));
        std::printf("synthetic model: max_level 0 refused: %s\n", error.c_str());
        CHECK(!gm::build_model_lut(model, 16, &error));
        CHECK(contains(error, "outside 1..15"));
        std::printf("synthetic model: max_level 16 refused: %s\n", error.c_str());

        CHECK(gm::build_model_lut(model, gm::kHudLutIdentityLevel, &error));
        for (int level = 0; level <= gm::kHudLutIdentityLevel; ++level) {
            for (int value = 0; value < 256; ++value) {
                const int expected =
                    (value * level + gm::kHudLutIdentityLevel / 2) /
                    gm::kHudLutIdentityLevel;
                CHECK_EQ(static_cast<int>(gm::map(model, level,
                                                  static_cast<std::uint8_t>(value))),
                         expected);
            }
        }
        CHECK_EQ(gm::map(model, 0, 200), 0);
        CHECK_EQ(gm::map(model, 11, 200), 200);
        CHECK_EQ(gm::map(model, 15, 200), 200); // clamped to max_level
        CHECK_EQ(gm::map(model, 5, 255), static_cast<std::uint8_t>((255 * 5 + 5) / 11));
        std::printf("synthetic model: max=11, map(0,200)=%d map(11,200)=%d "
                    "map(15,200)=%d map(5,255)=%d\n",
                    gm::map(model, 0, 200), gm::map(model, 11, 200),
                    gm::map(model, 15, 200), gm::map(model, 5, 255));
    }

    // build_hud_lut: the skip list and the refusals (docs/LUT_BUILDER.md §1/§3).
    {
        CHECK(gm::hud_lut_skip_slot(173));
        CHECK(gm::hud_lut_skip_slot(174));
        CHECK(gm::hud_lut_skip_slot(175));
        CHECK(gm::hud_lut_skip_slot(191));
        for (int index = 240; index <= 255; ++index) {
            CHECK(gm::hud_lut_skip_slot(index));
        }
        int skipped = 0;
        for (int index = 0; index < gm::kHudLutEntries; ++index) {
            if (gm::hud_lut_skip_slot(index)) {
                ++skipped;
            }
        }
        CHECK_EQ(skipped, 20);
        CHECK(!gm::hud_lut_skip_slot(0));
        CHECK(!gm::hud_lut_skip_slot(1));
        CHECK(!gm::hud_lut_skip_slot(172));
        CHECK(!gm::hud_lut_skip_slot(176));
        CHECK(!gm::hud_lut_skip_slot(190));
        CHECK(!gm::hud_lut_skip_slot(239));
        CHECK(!gm::hud_lut_skip_slot(-1));
        CHECK(!gm::hud_lut_skip_slot(256));
        std::printf("synthetic skip list: %d slots, 173..175+191+240..255, 0/1/239 "
                    "not skipped\n", skipped);

        // No palette at all and the wrong sizes are refused with text; the
        // table is zeroed, so a caller never draws a half-built one.
        gm::HudLut built = lut;
        CHECK(!gm::build_hud_lut({}, built, &error));
        CHECK(contains(error, "got none"));
        std::printf("synthetic build: no palette refused: %s\n", error.c_str());
        for (const auto& table : built.levels) {
            for (const std::uint8_t entry : table) {
                CHECK_EQ(entry, 0);
            }
        }
        constexpr std::size_t kPaletteSizes[2] = {gm::kHudLutPaletteBytes - 4,
                                                 gm::kHudLutPaletteBytes + 4};
        for (const std::size_t size : kPaletteSizes) {
            const std::vector<std::uint8_t> palette(size, 0x10);
            CHECK(!gm::build_hud_lut(palette, built, &error));
            CHECK(contains(error, std::to_string(size) + " bytes"));
            CHECK(contains(error, "expected 1024"));
            std::printf("synthetic build: %zu-byte palette refused: %s\n", size,
                        error.c_str());
        }

        // A synthetic palette still exercises the whole builder: every entry is
        // a slot of 1..254 or a skip slot, the skip slots never move, and two
        // runs agree (no hidden state).
        std::vector<std::uint8_t> synth(gm::kHudLutPaletteBytes);
        for (int index = 0; index < gm::kHudLutEntries; ++index) {
            const std::size_t at = static_cast<std::size_t>(index) * 4u;
            synth[at] = static_cast<std::uint8_t>(index);
            synth[at + 1] = static_cast<std::uint8_t>(255 - index);
            synth[at + 2] = static_cast<std::uint8_t>((index * 5) & 0xFF);
            synth[at + 3] = 255;
        }
        CHECK(gm::build_hud_lut(synth, built, &error));
        CHECK(error.empty());
        gm::HudLut again_built;
        CHECK(gm::build_hud_lut(synth, again_built, &error));
        CHECK(built == again_built);
        int skip_kept = 0;
        int lowest = 255;
        for (int level = 0; level < gm::kHudLutLevels; ++level) {
            for (int index = 0; index < gm::kHudLutEntries; ++index) {
                const std::uint8_t entry =
                    gm::map(built, level, static_cast<std::uint8_t>(index));
                if (gm::hud_lut_skip_slot(index)) {
                    CHECK_EQ(entry, static_cast<std::uint8_t>(index));
                    if (entry == static_cast<std::uint8_t>(index)) {
                        ++skip_kept;
                    }
                }
                // The search starts at slot 1, so index 0 is never produced.
                CHECK(entry >= 1);
                lowest = std::min(lowest, static_cast<int>(entry));
            }
        }
        CHECK_EQ(skip_kept, gm::kHudLutLevels * 20);
        CHECK_EQ(lowest, 1);
        CHECK_EQ(gm::map(built, 0, 255), static_cast<std::uint8_t>(255));
        CHECK_EQ(gm::map(built, 15, 255), static_cast<std::uint8_t>(255));
        std::printf("synthetic build: 16x256 from a synthetic palette, %d/320 skip "
                    "entries kept, min entry %d, 0 never produced\n",
                    skip_kept, lowest);
    }
}

// --------------------------------------------------------------------- real

void test_real() {
    const fs::path dump = find_dump();
    std::error_code ec;
    if (dump.empty() || !fs::is_regular_file(dump, ec) || ec) {
        std::printf("hud_lut: SKIP real dump (not found; set GM_HUD_LUT or run "
                    "tools/win/dump_lut.lua) path=%s\n",
                    dump.string().c_str());
        return;
    }

    gm::HudLut lut;
    gm::HudLutStats stats;
    std::string error;
    if (!gm::load_hud_lut(dump, lut, &error, &stats)) {
        CHECK(false);
        std::fprintf(stderr, "hud_lut: real dump: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(stats.bytes, gm::kHudLutBytes);
    CHECK_EQ(stats.levels, 16);
    CHECK_EQ(stats.entries, 256);
    // Pinned: a different dump would change every number below.
    const std::uint64_t hash = fnv1a64(
        reinterpret_cast<const std::uint8_t*>(&lut), sizeof(lut));
    CHECK_EQ(hash, 0x6330406254771f3dull);
    std::printf("real dump: %s, %zu bytes, FNV-1a64 0x%016llx\n", stats.source.c_str(),
                stats.bytes, static_cast<unsigned long long>(hash));

    // Palette: colour identity needs FRONTEND/GAMEPAL (res/bmp.h).
    std::vector<std::uint8_t> palette;
    {
        const fs::path game = find_game();
        std::string palette_error;
        const auto installed = gm::installed_palette(game, palette_error);
        if (installed && installed->size() == gm::kHudLutPaletteBytes) {
            palette = *installed;
            std::printf("real palette: %s (256 RGBA quads)\n", game.string().c_str());
        } else {
            std::printf("hud_lut: SKIP palette (game not installed: %s)\n",
                        palette_error.c_str());
        }
    }

    // The documented per-level counts, measured by an independent Python run
    // (docs/HUD_LUT.md §2 has the command): index identity and colour
    // identity.  Colour identity is the number docs/UPSCALE.md §3 quotes.
    constexpr int kIndexIdentity[gm::kHudLutLevels] = {21, 21, 21, 21, 21, 21, 21,
                                                       22, 23, 33, 68, 254, 130, 71,
                                                       46, 43};
    constexpr int kColourIdentity[gm::kHudLutLevels] = {22, 22, 22, 22, 22, 22, 22,
                                                        23, 24, 34, 70, 256, 132,
                                                        73, 48, 45};
    int identity[gm::kHudLutLevels] = {};
    int colour[gm::kHudLutLevels] = {};
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        const gm::HudLutLevel report = gm::level_report(lut, level, palette);
        identity[level] = report.identity;
        colour[level] = report.colour_identity;
        CHECK_EQ(report.identity, kIndexIdentity[level]);
        if (!palette.empty()) {
            CHECK_EQ(report.colour_identity, kColourIdentity[level]);
        }
        // Every dumped table is 1..255: index 0 is never written, white stays.
        CHECK_EQ(report.white, 1);
        CHECK_EQ(report.min, 1);
        CHECK_EQ(report.max, 255);
    }
    std::printf("real describe%s:\n%s\n", palette.empty() ? " (no palette)" : "",
                gm::describe(lut, palette).c_str());

    // The accepted criterion of this slice: levels 12..15 are not identity.
    if (!palette.empty()) {
        CHECK_EQ(colour[12], 132);
        CHECK_EQ(colour[13], 73);
        CHECK_EQ(colour[14], 48);
        CHECK_EQ(colour[15], 45);
        const std::string text = gm::describe(lut, palette);
        CHECK(contains(text, "colour-identity 132/256"));
        CHECK(contains(text, "colour-identity  73/256"));
        CHECK(contains(text, "colour-identity  48/256"));
        CHECK(contains(text, "colour-identity  45/256"));
    }
    CHECK_EQ(identity[12], 130);
    CHECK_EQ(identity[13], 71);
    CHECK_EQ(identity[14], 46);
    CHECK_EQ(identity[15], 43);

    // Level 11 is the identity of the curve: 254/256 by index (0 -> 1, both
    // black; 65 -> 48, the same RGB) and 256/256 by colour.
    CHECK_EQ(identity[11], 254);
    CHECK_EQ(gm::map(lut, 11, 0), static_cast<std::uint8_t>(1));
    CHECK_EQ(gm::map(lut, 11, 65), static_cast<std::uint8_t>(48));
    int index_diffs = 0;
    for (int index = 0; index < gm::kHudLutEntries; ++index) {
        if (gm::map(lut, 11, static_cast<std::uint8_t>(index)) != index) {
            ++index_diffs;
        }
    }
    CHECK_EQ(index_diffs, 2);
    if (!palette.empty()) {
        CHECK_EQ(colour[11], 256);
    }

    // Level 0 is black, not an identity: 236 entries hit index 1 (black) and
    // the 20 slots outside the palette ramps (173..175, 191, 240..255) keep
    // their colour.  Index 255 stays white at every level.
    CHECK_EQ(gm::map(lut, 0, 0), static_cast<std::uint8_t>(1));
    CHECK_EQ(gm::map(lut, 0, 255), static_cast<std::uint8_t>(255));
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        CHECK_EQ(gm::map(lut, level, 255), static_cast<std::uint8_t>(255));
    }
    if (!palette.empty()) {
        int black = 0;
        int untouched = 0;
        for (int index = 0; index < gm::kHudLutEntries; ++index) {
            const Rgb rgb = palette_rgb(palette, gm::map(lut, 0, static_cast<std::uint8_t>(index)));
            if (rgb.r == 0 && rgb.g == 0 && rgb.b == 0) {
                ++black;
            }
            if (gm::map(lut, 0, static_cast<std::uint8_t>(index)) == index) {
                ++untouched;
            }
        }
        CHECK_EQ(black, 236);
        CHECK_EQ(untouched, 21);
        std::printf("real level 0: %d/256 entries black, %d/256 indices untouched, "
                    "index 255 -> %d\n",
                    black, untouched, gm::map(lut, 0, 255));
    }

    // Model comparison: `(v*k + 5)/11` in the same index space.  Index match
    // counts entries equal as numbers, colour match counts entries whose RGB
    // equals the model of the source RGB - the only comparison that is fair
    // for index -> index tables (docs/UPSCALE.md §3.2).
    gm::HudLut model;
    CHECK(gm::build_model_lut(model, gm::kHudLutIdentityLevel, &error));
    std::printf("real model max=11: level index-match colour-match\n");
    int model_colour[gm::kHudLutLevels] = {};
    int model_index[gm::kHudLutLevels] = {};
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        int same_index = 0;
        int same_colour = 0;
        for (int value = 0; value < gm::kHudLutEntries; ++value) {
            const auto index = static_cast<std::uint8_t>(value);
            if (gm::map(lut, level, index) == gm::map(model, level, index)) {
                ++same_index;
            }
            if (!palette.empty()) {
                const Rgb rgb = palette_rgb(palette, value);
                const Rgb got = palette_rgb(palette, gm::map(lut, level, index));
                if (got.r == gm::map(model, level, static_cast<std::uint8_t>(rgb.r)) &&
                    got.g == gm::map(model, level, static_cast<std::uint8_t>(rgb.g)) &&
                    got.b == gm::map(model, level, static_cast<std::uint8_t>(rgb.b))) {
                    ++same_colour;
                }
            }
        }
        model_index[level] = same_index;
        model_colour[level] = same_colour;
        std::printf("real model: level %2d index-match %3d/256 colour-match %3d/256\n",
                    level, same_index, same_colour);
    }
    // The ends of the curve are where the model can be exact; level 11 is
    // 256/256 by colour (index 0 -> 1 is the same black), level 0 is 236/256,
    // not 256: the 20 out-of-ramp slots are left alone by the engine table,
    // while the model would blacken them.
    CHECK_EQ(model_index[11], 254);
    if (!palette.empty()) {
        CHECK_EQ(model_colour[11], 256);
        CHECK_EQ(model_colour[0], 236);
        for (int level = 1; level < gm::kHudLutIdentityLevel; ++level) {
            CHECK(model_colour[level] < 256);
        }
        for (int level = 12; level < gm::kHudLutLevels; ++level) {
            CHECK_EQ(model_colour[level], colour[level]);
        }
    }
    int middle_min = 256;
    int middle_max = 0;
    for (int level = 1; level < gm::kHudLutIdentityLevel; ++level) {
        middle_min = std::min(middle_min, model_colour[level]);
        middle_max = std::max(middle_max, model_colour[level]);
    }
    std::printf("real model: agreement with the dump is %d..%d/256 in the middle "
                "(levels 1..10), ends 236 and 256/256 by colour\n",
                middle_min, middle_max);

    // map: boundaries and the engine clamp 15, not 11.
    CHECK_EQ(gm::map(lut, 0, 3), lut.levels[0][3]);
    CHECK_EQ(gm::map(lut, 11, 3), lut.levels[11][3]);
    CHECK_EQ(gm::map(lut, 15, 3), lut.levels[15][3]);
    int clamp_same = 0;
    for (int index = 0; index < gm::kHudLutEntries; ++index) {
        const auto i = static_cast<std::uint8_t>(index);
        if (gm::map(lut, 99, i) == gm::map(lut, 15, i)) {
            ++clamp_same;
        }
    }
    CHECK_EQ(clamp_same, 256);
    CHECK(gm::map(lut, 15, 3) != gm::map(lut, 11, 3));
    std::printf("real map: index 3 -> level 0:%d 11:%d 15:%d 99:%d, clamp 99==15 on "
                "%d/256\n",
                gm::map(lut, 0, 3), gm::map(lut, 11, 3), gm::map(lut, 15, 3),
                gm::map(lut, 99, 3), clamp_same);

    // Determinism on the real file as well.
    gm::HudLut again;
    CHECK(gm::load_hud_lut(dump, again, &error));
    CHECK(lut == again);
    CHECK(gm::describe(lut, palette) == gm::describe(again, palette));
}

// ------------------------------------------------------------------- built

/// `build_hud_lut` from the installed palette: the dump is a check, not the
/// source (docs/LUT_BUILDER.md §4, docs/HUD_LUT.md §2b).
void test_build() {
    const fs::path game = find_game();
    std::string palette_error;
    const auto installed = gm::installed_palette(game, palette_error);
    if (!installed || installed->size() != gm::kHudLutPaletteBytes) {
        std::printf("hud_lut: SKIP built tables (no palette: %s)\n",
                    palette_error.c_str());
        return;
    }
    const std::vector<std::uint8_t>& palette = *installed;

    gm::HudLut built;
    std::string error;
    if (!gm::build_hud_lut(palette, built, &error)) {
        CHECK(false);
        std::fprintf(stderr, "hud_lut: build from %s: %s\n", game.string().c_str(),
                     error.c_str());
        return;
    }
    CHECK(error.empty());
    const std::uint64_t hash =
        fnv1a64(reinterpret_cast<const std::uint8_t*>(&built), sizeof(built));
    std::printf("built: 16x256 from %s (256 RGBA quads), FNV-1a64 0x%016llx\n",
                game.string().c_str(), static_cast<unsigned long long>(hash));

    constexpr int kIndexIdentity[gm::kHudLutLevels] = {21, 21, 21, 21, 21, 21, 21,
                                                       22, 23, 33, 68, 254, 130, 71,
                                                       46, 43};
    constexpr int kColourIdentity[gm::kHudLutLevels] = {22, 22, 22, 22, 22, 22, 22,
                                                        23, 24, 34, 70, 256, 132,
                                                        73, 48, 45};

    // Properties pinned before the dump is looked at: they hold for any build
    // from this palette and are the fallback when no dump is on the machine.
    int colour[gm::kHudLutLevels] = {};
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        const gm::HudLutLevel report = gm::level_report(built, level, palette);
        colour[level] = report.colour_identity;
        CHECK_EQ(report.identity, kIndexIdentity[level]);
        CHECK_EQ(report.colour_identity, kColourIdentity[level]);
        CHECK_EQ(report.white, 1);
        CHECK_EQ(report.min, 1);
        CHECK_EQ(report.max, 255);
    }
    CHECK_EQ(colour[11], 256); // level 11: colour identity on all 256 entries
    CHECK_EQ(colour[12], 132);
    CHECK_EQ(colour[13], 73);
    CHECK_EQ(colour[14], 48);
    CHECK_EQ(colour[15], 45);

    // Level 0: 236 entries land on black and the 20 skip slots keep their
    // index; the 21st index identity is slot 1, which is black and nearest.
    int black = 0;
    int untouched = 0;
    for (int index = 0; index < gm::kHudLutEntries; ++index) {
        const Rgb rgb =
            palette_rgb(palette, gm::map(built, 0, static_cast<std::uint8_t>(index)));
        if (rgb.r == 0 && rgb.g == 0 && rgb.b == 0) {
            ++black;
        }
        if (gm::hud_lut_skip_slot(index) &&
            gm::map(built, 0, static_cast<std::uint8_t>(index)) ==
                static_cast<std::uint8_t>(index)) {
            ++untouched;
        }
    }
    CHECK_EQ(black, 236);
    CHECK_EQ(untouched, 20);
    std::printf("built: level 11 colour identity %d/256, level 0 %d/256 black and "
                "%d/256 skip slots untouched\n",
                colour[11], black, untouched);

    // Determinism: the builder has no state, two runs are the same table.
    gm::HudLut again;
    CHECK(gm::build_hud_lut(palette, again, &error));
    CHECK(built == again);

    // The product is rounded to `double`, not kept in x87 extended precision.
    // Slot 54 is `(44,5,5)` in six bits; at level 3 the exact `44 * 3/11` sits
    // just below 12, so an extended-precision truncation would target 11 and
    // land on slot 48, while `double` rounds the product to 12 and lands on 49.
    // The dump says 49 (36 of 3776 slots separate the two models), so this pin
    // is what keeps the model honest (docs/HUD_LUT.md §2b).
    CHECK_EQ(static_cast<int>(palette[54 * 4 + 0]) >> 2, 44);
    CHECK_EQ(gm::map(built, 3, static_cast<std::uint8_t>(54)),
             static_cast<std::uint8_t>(49));
    std::printf("built: level 3 slot 54 (6-bit (44,5,5)) -> %d (extended precision would "
                "say 48)\n",
                static_cast<int>(gm::map(built, 3, static_cast<std::uint8_t>(54))));

    const fs::path dump = find_dump();
    std::error_code ec;
    if (dump.empty() || !fs::is_regular_file(dump, ec) || ec) {
        std::printf("hud_lut: SKIP built-vs-dump comparison (no dump; set GM_HUD_LUT "
                    "or run tools/win/dump_lut.lua); pinned properties above still "
                    "checked\n");
        return;
    }
    gm::HudLut dumped;
    if (!gm::load_hud_lut(dump, dumped, &error)) {
        CHECK(false);
        std::fprintf(stderr, "hud_lut: built-vs-dump: %s\n", error.c_str());
        return;
    }
    int same = 0;
    int first_level = -1;
    int first_index = -1;
    int per_level[gm::kHudLutLevels] = {};
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        for (int index = 0; index < gm::kHudLutEntries; ++index) {
            const auto i = static_cast<std::uint8_t>(index);
            if (gm::map(built, level, i) == gm::map(dumped, level, i)) {
                ++same;
                ++per_level[level];
            } else if (first_level < 0) {
                first_level = level;
                first_index = index;
            }
        }
    }
    CHECK_EQ(same, static_cast<int>(gm::kHudLutBytes));
    CHECK_EQ(hash, 0x6330406254771f3dull);
    CHECK(built == dumped);
    CHECK(first_level < 0);
    std::printf("built vs dump: identical bytes=%d/%zu, FNV 0x%016llx vs "
                "0x%016llx, first divergent byte: %s\n",
                same, gm::kHudLutBytes, static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(fnv1a64(
                    reinterpret_cast<const std::uint8_t*>(&dumped), sizeof(dumped))),
                first_level < 0 ? "none"
                                : ("level " + std::to_string(first_level) + " index " +
                                   std::to_string(first_index))
                                      .c_str());
    std::printf("built vs dump per level:");
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        std::printf(" %d:%d", level, per_level[level]);
    }
    std::printf("\n");
}

} // namespace

int main() {
    test_synthetic();
    test_build();
    test_real();
    if (g_failures != 0) {
        std::fprintf(stderr, "hud_lut: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hud_lut: ok\n");
    return 0;
}
