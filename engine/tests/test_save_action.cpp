// Save/load action (M8.7/M9.3 slice 2, docs/SAVE_ACTION.md).
//
// 1. `save`/`load`/`back`/`none` event routing and the counters: `back`/`none`
//    touch nothing, a closed menu or an out-of-range target is refused.
// 2. A `save` event on an occupied slot of the shipped `SAVEGAME` writes
//    `<out_dir>/saved0.sbt` byte for byte equal to `SAVEGAME/saved0.sbt`
//    (243368 bytes, SHA-256 printed), `stats().saves == 1`.
// 3. A `save` event on empty slot 5 creates no file (`refused_empty`) - no
//    template is invented.
// 4. A `load` event on slot 0 applies the save through `game/save_load.*`:
//    level `level0`, the saved0 feet/yaw, armor 10000/10000, shields 8000 x4,
//    ammo 2000/90, mask 0x3 (`stats().loads == 1`).
// 5. An empty/corrupt slot on load is refused with a text; a corrupt source is
//    refused on save too.
// 6. Writing into the game installation (its root or `SAVEGAME`) is refused,
//    and `saved0.sbt` is SHA-256-identical before and after.
// 7. A missing `out_dir` is created (model), two runs produce the same bytes,
//    and a second instance on the same input is deterministic.
//
// The shipped-save half needs `/mnt/d/games/Gunmetal 3dfx` and prints a skip
// line without it; the empty/corrupt directories are synthetic.
//
// The numbers printed at the end are quoted in docs/SAVE_ACTION.md §4.

#include "game/save_action.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/path.h" // resolve_ci
#include "ui/save_menu.h"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";
const fs::path kBase = "/tmp/opencode/gm_save_action";
constexpr std::size_t kShippedSize = 243368;
constexpr const char* kSaved0Sha =
    "e39c0c35c44af6b985cb272f52b1236404d4316b266dffc7ccf0548503b623ff";

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string in_quotes(const std::string& text) { return "\"" + text + "\""; }

std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out;
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return out;
}

bool write_file(const fs::path& path, std::string_view text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

/// Fresh `<base>/<name>/SAVEGAME`, no files inside.
fs::path make_empty_game(const std::string& name) {
    const fs::path root = kBase / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SAVEGAME", ec);
    return root;
}

std::size_t differing_bytes(const std::vector<std::uint8_t>& a,
                            const std::vector<std::uint8_t>& b) {
    const std::size_t common = std::min(a.size(), b.size());
    std::size_t diff = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
    for (std::size_t i = 0; i < common; ++i) {
        if (a[i] != b[i]) {
            ++diff;
        }
    }
    return diff;
}

// --- SHA-256 (FIPS 180-4), only to print/compare the numbers in the doc -----

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

std::uint32_t rotr32(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex(std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff));
    }
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = chunk + 4 * i;
            w[i] = (static_cast<std::uint32_t>(msg[at]) << 24) |
                   (static_cast<std::uint32_t>(msg[at + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[at + 2]) << 8) |
                   static_cast<std::uint32_t>(msg[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    char buf[65];
    for (std::size_t i = 0; i < h.size(); ++i) {
        std::snprintf(buf + 8 * i, 9, "%08x", h[i]);
    }
    return std::string(buf, 64);
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// --------------------------------------------------------------------- cases

void test_none_and_closed() {
    gm::SaveMenu menu;
    gm::SaveAction action;
    std::string error = "untouched";

    CHECK(action.apply(menu, gm::SaveMenuEvent::none, kGame, kBase / "none", &error) ==
          gm::SaveActionResult::none);
    CHECK(error == "untouched"); // `none` changes neither error nor counters
    CHECK(action.apply(menu, gm::SaveMenuEvent::back, kGame, kBase / "back", &error) ==
          gm::SaveActionResult::none);
    CHECK(action.stats().saves == 0 && action.stats().loads == 0 &&
          action.stats().refusals == 0);
    CHECK(action.error().empty());

    // A closed menu has no target slot: an armed save/load is refused.
    CHECK(action.apply(menu, gm::SaveMenuEvent::save, kGame, kBase / "closed", &error) ==
          gm::SaveActionResult::refused_write);
    CHECK(action.stats().refusals == 1 && action.stats().saves == 0);
    CHECK(contains(error, "not open"));
    CHECK(action.error() == error);
    CHECK(action.apply(menu, gm::SaveMenuEvent::load, kGame, kBase / "closed", &error) ==
          gm::SaveActionResult::refused_read);
    CHECK(action.stats().refusals == 2);
    CHECK(!fs::exists(kBase / "closed"));
}

void test_empty_slot() {
    const fs::path game = make_empty_game("empty");
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    menu.move(5);
    CHECK(menu.selected() == 5 && menu.is_open());

    gm::SaveAction action;
    const fs::path out = game / "out";
    std::string error;
    CHECK(action.apply(menu, gm::SaveMenuEvent::save, game, out, &error) ==
          gm::SaveActionResult::refused_empty);
    CHECK(action.stats().saves == 0 && action.stats().loads == 0 &&
          action.stats().refusals == 1);
    CHECK(!fs::exists(out / "saved5.sbt"));
    CHECK(!fs::exists(out)); // refused before creating the output directory
    CHECK(contains(error, "saved5.sbt") && contains(error, "does not exist"));
    CHECK(action.error() == error);
    // The text mentions that we do not invent an empty template.
    CHECK(contains(error, "no empty template"));

    gm::SaveMenu load_menu;
    load_menu.open(gm::SaveMenuMode::load, game);
    CHECK(load_menu.selected() == 0);
    gm::SaveAction load_action;
    CHECK(load_action.apply(load_menu, gm::SaveMenuEvent::load, game, game / "out",
                            &error) == gm::SaveActionResult::refused_empty);
    CHECK(load_action.stats().refusals == 1 && load_action.stats().loads == 0);
    CHECK(contains(error, "saved0.sbt"));
    std::printf("save_action: empty slot 5 save + slot 0 load -> refused_empty, no file\n");
}

void test_corrupt_slot() {
    const fs::path game = make_empty_game("corrupt");
    CHECK(write_file(game / "SAVEGAME" / "saved0.sbt",
                     std::string(0x140, 'A'))); // no 0xff terminator / no header

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::load, game);
    CHECK(!menu.error().empty()); // the listing recorded the parser problem
    CHECK(!menu.at(0).occupied);

    gm::SaveAction action;
    std::string error;
    CHECK(action.apply(menu, gm::SaveMenuEvent::load, game, game / "out", &error) ==
          gm::SaveActionResult::refused_read);
    CHECK(action.stats().refusals == 1 && action.stats().loads == 0);
    CHECK(contains(error, "saved0.sbt") && contains(error, "parsed"));
    std::printf("save_action: corrupt saved0 load -> refused_read %s\n", in_quotes(error).c_str());

    // Saving an unreadable source is refused too: never copy garbage.
    gm::SaveMenu save_menu;
    save_menu.open(gm::SaveMenuMode::save, game);
    gm::SaveAction save_action;
    CHECK(save_action.apply(save_menu, gm::SaveMenuEvent::save, game, game / "out", &error) ==
          gm::SaveActionResult::refused_read);
    CHECK(save_action.stats().saves == 0 && save_action.stats().refusals == 1);
    CHECK(!fs::exists(game / "out" / "saved0.sbt"));
}

/// The shipped `saved0.sbt`: save -> byte-identical copy, load -> the
/// docs/SAVE_LOAD.md §5 numbers, install guard, missing out_dir, determinism.
void test_shipped_save() {
    const auto source = gm::resolve_ci(kGame, "SAVEGAME/saved0.sbt");
    if (!source) {
        std::printf("save_action: shipped saves not installed at %s, skipping\n",
                    kGame.string().c_str());
        return;
    }
    const std::vector<std::uint8_t> original = read_all(*source);
    CHECK(original.size() == kShippedSize);
    const std::string original_sha = sha256_hex(original);
    CHECK(original_sha == kSaved0Sha);

    // --- save: occupied slot 0 -> <out>/saved0.sbt ---------------------------
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, kGame);
    CHECK(menu.selected() == 0 && menu.at(0).occupied && menu.has_save_directory());
    const fs::path out = kBase / "write" / "nested" / "slot0"; // does not exist
    CHECK(!fs::exists(out));
    gm::SaveAction action;
    std::string error;
    CHECK(action.apply(menu, gm::SaveMenuEvent::save, kGame, out, &error) ==
          gm::SaveActionResult::saved);
    CHECK(error.empty() && action.error().empty());
    CHECK(action.stats().saves == 1 && action.stats().loads == 0 &&
          action.stats().refusals == 0);
    CHECK(fs::is_directory(out)); // a missing out_dir is created (model)
    const fs::path written_path = out / "saved0.sbt";
    CHECK(fs::is_regular_file(written_path));
    const std::vector<std::uint8_t> written = read_all(written_path);
    const std::string written_sha = sha256_hex(written);
    CHECK(written.size() == original.size());
    CHECK(written == original);
    CHECK(differing_bytes(written, original) == 0);
    CHECK(written_sha == original_sha);
    std::printf("save_action: save slot0 -> %zu bytes sha256 %s identical=%d (saves=%d)\n",
                written.size(), written_sha.c_str(), written == original ? 1 : 0,
                action.stats().saves);

    // --- determinism: a second instance + directory, same bytes --------------
    gm::SaveAction again;
    const fs::path out2 = kBase / "write" / "second";
    CHECK(again.apply(menu, gm::SaveMenuEvent::save, kGame, out2, &error) ==
          gm::SaveActionResult::saved);
    const std::vector<std::uint8_t> written2 = read_all(out2 / "saved0.sbt");
    CHECK(written2 == written);
    CHECK(sha256_hex(written2) == written_sha);
    CHECK(again.stats().saves == 1);

    // --- load: slot 0 -> save_load numbers -----------------------------------
    gm::SaveMenu load_menu;
    load_menu.open(gm::SaveMenuMode::load, kGame);
    CHECK(load_menu.selected() == 0 && load_menu.at(0).occupied);
    gm::SaveAction loader;
    CHECK(loader.apply(load_menu, gm::SaveMenuEvent::load, kGame, kBase / "load", &error) ==
          gm::SaveActionResult::loaded);
    CHECK(error.empty());
    CHECK(loader.stats().loads == 1 && loader.stats().saves == 0 &&
          loader.stats().refusals == 0);
    const gm::LoadedSave& loaded = loader.last_load();
    CHECK(loaded.player_applied);
    CHECK(loaded.level == "level0");
    CHECK(near(loaded.feet.x, 1116.173751898651, 1e-9));
    CHECK(near(loaded.feet.y, 798.2076214059642, 1e-9));
    CHECK(near(loaded.feet.z, 48.0, 1e-9));
    CHECK(near(loaded.yaw, 5.0736149946795805, 1e-9));
    CHECK(loaded.vitals.armor == 10000 && loaded.vitals.armor_max == 10000);
    CHECK(loaded.vitals.shield_max == 8000);
    CHECK((loaded.vitals.shields == std::array<std::int32_t, 4>{8000, 8000, 8000, 8000}));
    CHECK(loaded.arsenal.current == 0 && loaded.arsenal.mask == 0x3);
    CHECK(loaded.arsenal.ammo[0] == 2000 && loaded.arsenal.ammo[1] == 90);
    CHECK(loaded.blocks == 15);
    CHECK(loaded.sector_stuff == 256 && loaded.sector_stuff_active == 13);
    CHECK(!loaded.unsupported.empty());
    std::printf("save_action: load slot0 -> level=%s pos=(%.6f, %.6f, %.6f) yaw=%.6f "
                "armor=%d/%d shields=%d/%d/%d/%d ammo=%u,%u mask=0x%x loads=%d\n",
                loaded.level.c_str(), loaded.feet.x, loaded.feet.y, loaded.feet.z,
                loaded.yaw, loaded.vitals.armor, loaded.vitals.armor_max,
                loaded.vitals.shields[0], loaded.vitals.shields[1],
                loaded.vitals.shields[2], loaded.vitals.shields[3],
                loaded.arsenal.ammo[0], loaded.arsenal.ammo[1], loaded.arsenal.mask,
                loader.stats().loads);

    // --- the installation is read-only --------------------------------------
    const std::string sha_before = sha256_hex(read_all(*source));
    gm::SaveAction guard;
    const fs::path game_out = kGame;
    CHECK(guard.apply(menu, gm::SaveMenuEvent::save, kGame, game_out, &error) ==
          gm::SaveActionResult::refused_write);
    CHECK(contains(error, "game installation"));
    CHECK(guard.stats().saves == 0 && guard.stats().refusals == 1);
    CHECK(guard.apply(menu, gm::SaveMenuEvent::save, kGame, kGame / "SAVEGAME", &error) ==
          gm::SaveActionResult::refused_write);
    CHECK(guard.stats().refusals == 2);
    const std::string sha_after = sha256_hex(read_all(*source));
    CHECK(sha_after == sha_before);
    CHECK(sha_after == kSaved0Sha);
    CHECK(!fs::exists(fs::path(source->string() + ".part")));
    std::printf("save_action: install guard refused 2 writes, saved0 sha256 %s unchanged=%d\n",
                sha_after.c_str(), sha_after == sha_before ? 1 : 0);
}

} // namespace

int main() {
    std::error_code ec;
    fs::remove_all(kBase, ec);
    fs::create_directories(kBase, ec);

    test_none_and_closed();
    test_empty_slot();
    test_corrupt_slot();
    test_shipped_save();

    std::printf("save_action: %d checks, ok (saves/loads/refusals counted per instance)\n",
                g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "save_action: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
