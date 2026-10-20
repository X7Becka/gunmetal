// HUD film playback: the 91 Hz clock, .fdx count/fps, start_frame/loop/hold
// and the suffix -> index-line binding.
//
// Synthetic 2-frame films pin the transitions and boundaries; the shipped case
// plays AMMO4.SOF (ammo.fdx line 4, 131 frames @ 24) at ticks 0/91/182 and
// compares the composited 320x200 RGB frames against FNV-1a64 goldens produced
// independently with gm/sof.py + gm/bmp.py (docs/HUD_PLAYER.md §4).
// WEAPON8.SOF -> weapon.fdx line 8 (137 frames, the only non-131 line) is the
// live proof of the numeric-suffix binding.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "render/hud.h"
#include "render/hud_player.h"
#include "render/render.h"
#include "res/bmp.h"
#include "res/sof.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::vector<std::uint8_t> make_palette() {
    std::vector<std::uint8_t> palette(256 * 4);
    for (int i = 0; i < 256; ++i) {
        palette[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(i);
        palette[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(255 - i);
        palette[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>((i * 7) & 0xff);
        palette[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return palette;
}

gm::RenderResult make_target(int width, int height, int fill) {
    gm::RenderResult target;
    target.width = width;
    target.height = height;
    target.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3,
                      static_cast<std::uint8_t>(fill));
    return target;
}

/// One flat 128x64 tile of *index*.
std::vector<std::uint8_t> flat_tile(std::uint8_t index) {
    return std::vector<std::uint8_t>(128 * 64, index);
}

gm::Sof make_sof(const std::vector<std::vector<std::uint8_t>>& tiles) {
    gm::Sof film;
    for (const auto& pixels : tiles) {
        const auto tile = gm::encode_sof_tile(pixels);
        film.data.insert(film.data.end(), tile.begin(), tile.end());
    }
    return film;
}

gm::Fdx make_fdx(std::uint32_t count, std::uint32_t fps) {
    std::string error;
    auto fdx = gm::parse_fdx(gm::build_fdx({{count, fps, 0}}), error);
    check(fdx.has_value(), "synthetic fdx parses: " + error);
    return fdx ? *fdx : gm::Fdx{};
}

/// 2 tiles (indices 1, 2), one .fdx record: the smallest playable film.
gm::HudFilm make_two_frame(std::uint32_t fps) {
    gm::HudFilm film;
    film.sof = make_sof({flat_tile(1), flat_tile(2)});
    film.fdx = make_fdx(2, fps);
    film.count = 2;
    film.fps = fps;
    film.start_frame = 0;
    film.loop = true;
    return film;
}

void check_frame(const gm::HudFilm& film, std::uint64_t tick, std::size_t expected,
                 const std::string& what) {
    std::string error;
    const auto got = gm::frame_at(film, tick, error);
    check(got.has_value(), what + ": frame_at succeeds (" + error + ")");
    if (got) {
        check(got->frame == expected, what + ": frame " + std::to_string(got->frame) +
                                          " == " + std::to_string(expected));
    }
}

void test_basics() {
    // --- 15 fps over 91 ticks: frame = tick * 15 / 91.
    {
        auto film = make_two_frame(15);
        check_frame(film, 0, 0, "15 fps tick 0");
        check_frame(film, 6, 0, "15 fps tick 6 (90/91)");
        check_frame(film, 7, 1, "15 fps tick 7 (105/91)");
        check_frame(film, 13, 0, "15 fps tick 13 wraps (195/91=2)");
        check_frame(film, 91, 1, "15 fps tick 91 (one second, 15 frames)");
        check_frame(film, 182, 0, "15 fps tick 182 (two seconds, 30 frames)");
        std::string error;
        const auto got = gm::frame_at(film, 91, error);
        check(got && !got->ended, "15 fps loop never reports ended");
        check(error.empty(), "frame_at clears the error");
    }

    // --- HUD default start_frame = 1: the first frame on screen is tile 1.
    {
        auto film = make_two_frame(15);
        film.start_frame = 1;
        check_frame(film, 0, 1, "start_frame 1 at tick 0");
        check_frame(film, 6, 1, "start_frame 1 at tick 6");
        check_frame(film, 7, 0, "start_frame 1 at tick 7 wraps to 0");
        check_frame(film, 91, 0, "start_frame 1 at tick 91");
    }

    // --- 91 fps is one frame per tick.
    {
        auto film = make_two_frame(91);
        check_frame(film, 0, 0, "91 fps tick 0");
        check_frame(film, 1, 1, "91 fps tick 1");
        check_frame(film, 2, 0, "91 fps tick 2 wraps");
        check_frame(film, 3, 1, "91 fps tick 3");
        film.start_frame = 1;
        check_frame(film, 0, 1, "91 fps start_frame 1 at tick 0");
        check_frame(film, 1, 0, "91 fps start_frame 1 at tick 1");
    }

    // --- non-looping film holds the last frame and reports ended.
    {
        auto film = make_two_frame(15);
        film.start_frame = 1;
        film.loop = false;
        std::string error;
        const auto first = gm::frame_at(film, 0, error);
        check(first.has_value() && first->frame == 1, "hold: tick 0 is the start frame");
        check(first.has_value() && !first->ended, "hold: start frame is not ended");
        const auto last = gm::frame_at(film, 6, error);
        check(last.has_value() && last->frame == 1 && !last->ended,
              "hold: final frame still plays");
        const auto past = gm::frame_at(film, 7, error);
        check(past.has_value() && past->frame == 1, "hold: clamps to count-1");
        check(past.has_value() && past->ended, "hold: past the end reports ended");
        const auto far = gm::frame_at(film, 100000, error);
        check(far.has_value() && far->frame == 1 && far->ended, "hold: stays clamped");
    }

    // --- count = 1: start_frame 1 wraps to 0, a lone frame still plays.
    {
        gm::HudFilm film;
        film.sof = make_sof({flat_tile(3)});
        film.fdx = make_fdx(1, 24);
        film.count = 1;
        film.fps = 24;
        film.start_frame = 1;
        check_frame(film, 0, 0, "single frame at tick 0");
        check_frame(film, 1000, 0, "single frame at tick 1000");
    }
}

void test_validation() {
    const auto palette = make_palette();

    // --- no index record: an empty Fdx is rejected by frame_at/draw.
    {
        gm::HudFilm film;
        film.sof = make_sof({flat_tile(1)});
        std::string error;
        check(!gm::frame_at(film, 0, error), "missing index rejected");
        check(!error.empty(), "missing index sets an error");
        auto target = make_target(8, 8, 0);
        check(!gm::draw(film, target, palette, 0, 0, false, 1, 1.0, 0, error),
              "draw without an index rejected");
    }

    // --- fps 0 / absurd fps / count 0 / count beyond the tiles.
    {
        auto film = make_two_frame(15);
        film.fps = 0;
        std::string error;
        check(!gm::frame_at(film, 0, error), "fps 0 rejected");
        check(error.find("fps") != std::string::npos, "fps 0 error names fps");

        film.fps = gm::kFdxMaxFps + 1;
        check(!gm::frame_at(film, 0, error), "fps above the cap rejected");

        film = make_two_frame(15);
        film.count = 0;
        check(!gm::frame_at(film, 0, error), "count 0 rejected");
        check(error.find("count") != std::string::npos, "count 0 error names count");

        film.count = 3; // two tiles only
        check(!gm::frame_at(film, 0, error), "count above tiles rejected");
        check(error.find("outside film") != std::string::npos,
              "count above tiles error says 'outside film'");
    }

    // --- tick overflow is a clean error, not a wrapped frame.
    {
        auto film = make_two_frame(24);
        std::string error;
        const auto huge = std::numeric_limits<std::uint64_t>::max();
        check(!gm::frame_at(film, huge, error), "overflowing tick rejected");
        check(error.find("overflow") != std::string::npos, "overflow error says overflow");

        const auto target = make_target(8, 8, 5);
        auto copy = target;
        check(!gm::draw(film, copy, palette, 0, 0, false, 1, 1.0, huge, error),
              "draw with an overflowing tick rejected");
        check(copy.rgb == target.rgb, "overflowing tick leaves the buffer alone");
    }

    // --- negative origin, bad scale, NaN opacity are rejected before writing.
    {
        auto film = make_two_frame(15);
        auto target = make_target(8, 8, 7);
        const auto before = target;
        std::string error;
        check(!gm::draw(film, target, palette, -1, 0, false, 1, 1.0, 0, error),
              "negative x rejected");
        check(error.find("negative") != std::string::npos, "negative x error is explicit");
        check(!gm::draw(film, target, palette, 0, -3, false, 1, 1.0, 0, error),
              "negative y rejected");
        check(!gm::draw(film, target, palette, 0, 0, false, 0, 1.0, 0, error),
              "scale 0 rejected by the blitter");
        check(!gm::draw(film, target, palette, 0, 0, true, 1,
                        std::numeric_limits<double>::quiet_NaN(), 0, error),
              "NaN opacity rejected");
        check(target.rgb == before.rgb, "rejected draws leave the buffer alone");
        check(gm::draw(film, target, palette, 0, 0, false, 1, 1.0, 7, error),
              "valid draw succeeds (" + error + ")");
        check(error.empty(), "valid draw clears the error");
        check(target.rgb[0] == 2 && target.rgb[1] == 253 && target.rgb[2] == 14,
              "tick 7 shows tile 1 (index 2 -> palette 2,253,14)");
    }
}

/// Filesystem-level loading: case-insensitive names, suffix -> line, .idx
/// fallback, Default -> line 0 and the missing-index/missing-film errors.
void test_loading(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Film", ec);

    const auto two_frames = make_sof({flat_tile(1), flat_tile(2)});
    write_file(root / "Film" / "WEAPON8.SOF", two_frames.to_bytes());
    write_file(root / "Film" / "AMMO1.SOF", two_frames.to_bytes());
    write_file(root / "Film" / "SHIELD0.SOF", two_frames.to_bytes());
    write_file(root / "Film" / "Default.sof", two_frames.to_bytes());
    write_file(root / "Film" / "NOIDX.SOF", two_frames.to_bytes());
    write_file(root / "Film" / "WEAPON9.SOF", two_frames.to_bytes());
    // 9 lines (0..8); only line 8 differs, like weapon.fdx:8 = 137.
    std::vector<gm::FdxEntry> weapon;
    for (int i = 0; i < 9; ++i) {
        weapon.push_back({static_cast<std::uint32_t>(i == 8 ? 2 : 1), 15,
                          static_cast<std::size_t>(i)});
    }
    write_file(root / "Film" / "weapon.fdx", gm::build_fdx(weapon));
    write_file(root / "Film" / "default.fdx", gm::build_fdx({{2, 30, 0}}));

    std::string error;
    const auto weapon8 = gm::load_hud_film(root, "WEAPON8", error);
    check(weapon8.has_value(), "synthetic WEAPON8 loads (" + error + ")");
    if (weapon8) {
        check(weapon8->line == 8, "WEAPON8 -> line 8");
        check(weapon8->count == 2, "WEAPON8 -> line 8 count");
        check(weapon8->fps == 15, "WEAPON8 -> line 8 fps");
        check(weapon8->sof.tiles() == 2, "WEAPON8 -> two tiles");
        check(weapon8->start_frame == 1, "HUD loader starts on frame 1");
        check(weapon8->loop, "HUD loader loops");
        check(weapon8->fdx.entries.size() == 9, "index kept for inspection");
    }

    // Same call, lower-case and with an extension: case-insensitive on Linux.
    check(gm::load_hud_film(root, "weapon8.sof", error).has_value(),
          "lower-case name with extension resolves (" + error + ")");

    // Suffix past the last physical line.
    check(!gm::load_hud_film(root, "WEAPON9", error), "line 9 outside a 9-line index");
    check(error.find("outside") != std::string::npos, "line error says 'outside'");

    // No numeric suffix (and not Default).
    check(!gm::load_hud_film(root, "NOIDX", error), "name without a suffix rejected");
    check(error.find("suffix") != std::string::npos, "suffix error is explicit");

    // Film without any sibling index (neither .fdx nor .idx).
    check(!gm::load_hud_film(root, "SHIELD0", error), "no index for SHIELD0");
    check(error.find("SHIELD0") != std::string::npos, "missing index error names the film");

    // .idx fallback of the engine's find_fdx.
    write_file(root / "Film" / "shield.idx", gm::build_fdx({{2, 40, 0}}));
    const auto shield = gm::load_hud_film(root, "SHIELD0", error);
    check(shield.has_value() && shield->fps == 40,
          "legacy .idx fallback uses its own record (" + error + ")");

    // Default has no digits and is loaded with index 0.
    const auto fallback = gm::load_hud_film(root, "Default", error);
    check(fallback.has_value(), "Default loads (" + error + ")");
    if (fallback) {
        check(fallback->line == 0, "Default -> line 0");
        check(fallback->fps == 30, "Default -> fps from line 0");
    }

    // Missing film file.
    check(!gm::load_hud_film(root, "WEAPON7", error), "missing WEAPON7.sof rejected");
    check(error.find("not found") != std::string::npos, "missing film error says 'not found'");

    // Empty and oversized names never touch the filesystem.
    check(!gm::load_hud_film(root, "", error), "empty name rejected");
    check(!gm::load_hud_film(root, std::string(gm::kHudMaxNameLength + 1, 'A'), error),
          "oversized name rejected");

    fs::remove_all(root, ec);
}

/// Shipped films: AMMO4 at ticks 0/91/182 and the WEAPON8 binding.
/// Returns false when the installation is absent (the case is skipped).
bool test_game(const fs::path& game) {
    std::error_code ec;
    const fs::path ammo = game / "Film" / "AMMO4.SOF";
    const fs::path palette_path = game / "Film" / "Kflm0000.bmp";
    if (!fs::is_regular_file(ammo, ec) || !fs::is_regular_file(palette_path, ec)) {
        std::cout << "hud_player: game not installed, shipped-film case skipped\n";
        return false;
    }

    const auto bmp_bytes = read_file(palette_path);
    std::string error;
    const auto bmp = gm::parse_bmp(bmp_bytes, error);
    check(bmp.has_value() && bmp->palette.size() == 1024,
          "Kflm0000.bmp carries 256 colours (" + error + ")");
    if (!bmp) {
        return true;
    }

    const auto film = gm::load_hud_film(game, "AMMO4", error);
    check(film.has_value(), "AMMO4 loads (" + error + ")");
    if (!film) {
        return true;
    }
    check(film->line == 4, "AMMO4 -> ammo.fdx line 4");
    check(film->count == 131, "AMMO4 -> count 131");
    check(film->fps == 24, "AMMO4 -> fps 24");
    check(film->sof.tiles() == 132, "AMMO4 -> 132 tiles (one extra, count = tiles - 1)");
    check(film->start_frame == 1 && film->loop, "AMMO4 uses the HUD start=1/loop=1 policy");

    // tick * 24 / 91 + 1 over count 131: 0 -> 1, 91 -> 25, 182 -> 49.
    const std::uint64_t ticks[3] = {0, 91, 182};
    const std::size_t expected[3] = {1, 25, 49};
    const std::uint64_t goldens[3] = {0xaaf521e923f31099ull, 0xeeb1a6677e96f650ull,
                                      0x505b37daeecc1c76ull};
    for (int i = 0; i < 3; ++i) {
        const auto got = gm::frame_at(*film, ticks[i], error);
        check(got.has_value(), "AMMO4 frame_at tick " + std::to_string(ticks[i]) + " (" +
                                   error + ")");
        if (!got) {
            continue;
        }
        check(got->frame == expected[i],
              "AMMO4 tick " + std::to_string(ticks[i]) + " -> frame " +
                  std::to_string(expected[i]) + " (got " + std::to_string(got->frame) + ")");
        auto target = make_target(320, 200, 0);
        check(gm::draw(*film, target, bmp->palette, 14, 125, false, 1, 1.0, ticks[i], error),
              "AMMO4 draw tick " + std::to_string(ticks[i]) + " (" + error + ")");
        const std::uint64_t hash = fnv1a64(target.rgb);
        check(hash == goldens[i],
              "AMMO4 tick " + std::to_string(ticks[i]) + " FNV-1a64 " +
                  std::to_string(goldens[i]) + " (got " + std::to_string(hash) + ")");
    }

    // 320x200 keeps row 124 (above the overlay) untouched.
    auto target = make_target(320, 200, 0);
    check(gm::draw(*film, target, bmp->palette, 14, 125, false, 1, 1.0, 0, error),
          "AMMO4 draw for the row check (" + error + ")");
    bool row_clean = true;
    for (int x = 0; x < 320; ++x) {
        const std::size_t offset = static_cast<std::size_t>(124 * 320 + x) * 3;
        row_clean = row_clean && target.rgb[offset] == 0 && target.rgb[offset + 1] == 0 &&
                    target.rgb[offset + 2] == 0;
    }
    check(row_clean, "AMMO4 leaves row 124 untouched");

    // WEAPON8: the suffix selects line 8, the only 137-frame record.
    const auto weapon8 = gm::load_hud_film(game, "WEAPON8", error);
    check(weapon8.has_value(), "WEAPON8 loads (" + error + ")");
    if (weapon8) {
        check(weapon8->line == 8, "WEAPON8 -> line 8");
        check(weapon8->count == 137, "WEAPON8 -> count 137");
        check(weapon8->fps == 24, "WEAPON8 -> fps 24");
        check(weapon8->sof.tiles() == 137, "WEAPON8 -> 137 tiles (count == tiles)");
        check_frame(*weapon8, 91, 25, "WEAPON8 tick 91");
        check_frame(*weapon8, 0, 1, "WEAPON8 tick 0");
    }

    // Scripted film: no sibling index -> explicit refusal.
    check(!gm::load_hud_film(game, "Kelfilm", error), "Kelfilm has no .fdx");
    check(!error.empty(), "missing Kelfilm index sets an error");

    // Line exists in weapon.fdx but the file does not.
    check(!gm::load_hud_film(game, "WEAPON20", error), "WEAPON20.sof does not exist");
    check(error.find("not found") != std::string::npos, "missing WEAPON20 says 'not found'");

    // Case-insensitive path resolution against the shipped mixed-case names.
    check(gm::load_hud_film(game, "ammo4.sof", error).has_value(),
          "ammo4.sof resolves on a case-sensitive filesystem (" + error + ")");
    return true;
}

} // namespace

int main() {
    test_basics();
    test_validation();

    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        tmp = "/tmp";
    }
    test_loading(tmp / "gm_hud_player_tests");

    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const bool shipped = test_game(game);

    if (failures == 0) {
        std::cout << "hud_player: clock + binding ok";
        if (shipped) {
            std::cout << ", AMMO4 FNV aaf521e923f31099 / eeb1a6677e96f650 / "
                         "505b37daeecc1c76";
        }
        std::cout << "\n";
    }
    return failures == 0 ? 0 : 1;
}
