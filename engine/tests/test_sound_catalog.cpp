// Sample name resolution (`M7.6`): `SOUND_SAMPLE_TABLE` -> `sounds/SOU.IDX`.
//
// `docs/SOUND_NAMES.md`: the shipped tables name 5800 slots, 528 distinct names
// after the engine's uppercase fold, and the original resolves **all** of them
// (no missing names — the "95" in earlier notes are the names that also have a
// line in `sounds.sub`, which v1.36 never loads).  A miss must stay a quiet
// `nullopt`: playback rejects it and nothing crashes.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "res/sbt.h"
#include "res/sound_catalog.h"

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

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    std::error_code ec;
    if (!fs::is_regular_file(game / "sounds" / "SOU.IDX", ec)) {
        std::printf("sound_catalog: game not installed, skipped\n");
        return 0;
    }

    std::string error;
    const auto catalog = gm::SoundCatalog::open(game, error);
    if (!catalog) {
        std::printf("sound_catalog: %s\n", error.c_str());
        return 1;
    }
    std::printf("sound_catalog: %zu samples, music fallback: %s\n", catalog->size(),
                catalog->has_music() ? "yes" : "no");

    // ---- the corpus: every name in every level's type-12 table must resolve --
    long slots = 0;
    long unresolved = 0;
    std::set<std::array<char, 8>> keys;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(game, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sbt") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    CHECK(files.size() == 36);
    for (const auto& path : files) {
        const auto bytes = read_file(path);
        std::string parse_error;
        const auto level = gm::parse_sbt(bytes, parse_error);
        if (!level) {
            std::fprintf(stderr, "FAIL %s: %s\n", path.filename().c_str(),
                         parse_error.c_str());
            ++g_failures;
            continue;
        }
        if (!level->sound_samples) {
            std::fprintf(stderr, "FAIL %s: no SOUND_SAMPLE_TABLE\n",
                         path.filename().c_str());
            ++g_failures;
            continue;
        }
        for (const auto& name : level->sound_samples->names) {
            if (name.empty()) {
                continue;
            }
            ++slots;
            keys.insert(gm::SoundCatalog::key_of(name));
            if (!catalog->find(name)) {
                if (unresolved < 5) {
                    std::fprintf(stderr, "FAIL unresolved name '%s' in %s\n",
                                 name.c_str(), path.filename().c_str());
                }
                ++unresolved;
            }
        }
    }
    std::printf("sound_catalog: %ld named slots, %zu unique keys, %ld unresolved\n",
                slots, keys.size(), unresolved);
    CHECK(unresolved == 0);
    // Pinned by tools/re/sound_names.py --report (docs/SOUND_NAMES.md §4).
    CHECK(slots == 5800);
    CHECK(keys.size() == 528);

    // ---- lookup rules ------------------------------------------------------
    const auto alarm = catalog->find("alarmk");
    CHECK(alarm.has_value());
    if (alarm) {
        CHECK(alarm->name == "ALARMK");
        CHECK(alarm->archive == "sounds");
        CHECK(alarm->size == 13619);
        CHECK(alarm->offset == 24369);
    }

    // 0x480180 uppercases: the table stores mixed case, the index does not.
    const auto upper = catalog->find("ALARMK");
    CHECK(upper.has_value() && upper->name == "ALARMK");
    const auto mixed = catalog->find("AlarmK");
    CHECK(mixed.has_value() && mixed->name == "ALARMK");

    // 0x40ade0 cuts the directory at the last '\\' or ':'.
    const auto path_name = catalog->find("C:\\Sounds\\alarmk");
    CHECK(path_name.has_value() && path_name->name == "ALARMK");
    const auto colon_name = catalog->find("sounds:alarmk");
    CHECK(colon_name.has_value() && colon_name->name == "ALARMK");

    // 0x40d1f0 keeps exactly 8 bytes: a 9-char name loses its tail, so
    // "alarmkbig" becomes ALARMKBI and does not silently alias ALARMK.
    CHECK(gm::SoundCatalog::key_of("alarmkbig") ==
          (std::array<char, 8>{'A', 'L', 'A', 'R', 'M', 'K', 'B', 'I'}));
    CHECK(!catalog->find("alarmkbig").has_value());

    // Missing names are a normal miss (the original returns -1, plays silence).
    CHECK(!catalog->find("no such sample").has_value());
    CHECK(!catalog->find("").has_value());

    // 0x40ae3c: a name that only exists in MUSIC/SOU.IDX resolves through the
    // fallback archive (DM1A01 is music-only; docs/SOUND_NAMES.md §2).
    if (catalog->has_music()) {
        const auto music = catalog->find("dm1a01");
        CHECK(music.has_value());
        if (music) {
            CHECK(music->archive == "music");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "sound_catalog: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sound_catalog: ok\n");
    return 0;
}
