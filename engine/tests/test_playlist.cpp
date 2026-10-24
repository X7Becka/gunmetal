// `MUSIC/*.sog` playlists resolved against `MUSIC/SOU.IDX`
// (`audio/playlist.h`, docs/PLAYLIST.md, docs/FORMATS.md §9).
//
// The module is pure data: no decode, no mixer, no platform audio.  The test
// covers:
//
//   * the line model (CRLF/LF, blank lines, surrounding whitespace, trailing
//     `\r`) and the refusals: a missing file, a non-`.sog` path, an empty or
//     all-blank file, a 256-byte name, an archive with no entries and a
//     `MUSIC/SOU.IDX` that does not exist — each with a text and `out`
//     untouched;
//   * the two name-length boundaries (`kArchiveNameBytes` 8: loads, cannot
//     resolve; `kMaxNameBytes` 255: the last accepted length);
//   * the real installation: 651 files (598 `.sog` + 53 `.SOG`), first/last
//     three names, five playlists with their found/missing/duplicate numbers
//     and the index+size of their first samples, and the whole corpus
//     aggregate (5211 names, 5088 resolved, 123 misses, 95 distinct absent
//     names with the top five, 998 duplicates, Σ bytes) plus the
//     case-sensitive count that proves the ASCII fold is needed;
//   * determinism of `load_playlist`/`resolve_playlist` and the equality of
//     `playlist_files(game)` with `playlist_files(game/MUSIC)`.
//
// Numbers are printed, not only asserted.

#include "audio/playlist.h"
#include "res/bigfile.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

std::optional<fs::path> game_dir() {
    const std::array<fs::path, 2> candidates{
        fs::path("/mnt/d/games/Gunmetal 3dfx"),
        fs::path(std::getenv("HOME") != nullptr ? std::getenv("HOME") : "") /
            ".cache/gunmetal-re/game"};
    for (const auto& path : candidates) {
        std::error_code ec;
        if (fs::is_directory(path / "MUSIC", ec)) {
            return path;
        }
    }
    return std::nullopt;
}

/// Scratch root: `/var/tmp` is the project's scratch disk (`/tmp` is tmpfs),
/// `GM_PLAYLIST_TMPDIR` overrides it, the system temp dir is the last resort.
fs::path temp_root() {
    std::string base = "/var/tmp";
    if (const char* env = std::getenv("GM_PLAYLIST_TMPDIR"); env != nullptr && *env != '\0') {
        base = env;
    }
    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        base = fs::temp_directory_path(ec).string();
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&g_checks)));
    const fs::path root = fs::path(base) / (std::string("gm_playlist") + suffix);
    fs::create_directories(root, ec);
    return root;
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string repeat(char c, std::size_t count) {
    return std::string(count, c);
}

std::string folded(std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

std::string upper_of(std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return key;
}

/// The 8-byte archive-field name and the 255-byte accepted maximum.
std::string boundary_names() {
    return repeat('N', gm::audio::kArchiveNameBytes) + "\r\n" +
           repeat('L', gm::audio::kMaxNameBytes) + "\r\n";
}

// ------------------------------------------------------------ negatives ----

void test_line_model(const fs::path& root) {
    std::string error;

    // CRLF, blank lines, surrounding blanks: only the two names survive.
    const fs::path mixed = root / "mixed.sog";
    CHECK(write_text(mixed, " A \r\n\r\n\tB\t\r\n\r\n"));
    gm::audio::Playlist playlist;
    CHECK(gm::audio::load_playlist(mixed, playlist, &error));
    CHECK(error.empty());
    CHECK_EQ(playlist.entries.size(), std::size_t{2});
    CHECK_EQ(playlist.entries[0].name, std::string("A"));
    CHECK_EQ(playlist.entries[1].name, std::string("B"));
    CHECK(!playlist.resolved);
    CHECK_EQ(playlist.entries[0].index_in_music, -1);
    CHECK_EQ(playlist.entries[0].bytes, 0);
    CHECK(!playlist.entries[0].found);
    printf("line model: CRLF/blank/whitespace -> %zu names (\"%s\", \"%s\")\n",
           playlist.entries.size(), playlist.entries[0].name.c_str(),
           playlist.entries[1].name.c_str());

    // LF only and a missing final newline parse the same way.
    const fs::path lf = root / "lf.sog";
    CHECK(write_text(lf, "ALPHA\nBETA"));
    gm::audio::Playlist lf_list;
    CHECK(gm::audio::load_playlist(lf, lf_list, &error));
    CHECK_EQ(lf_list.entries.size(), std::size_t{2});
    CHECK_EQ(lf_list.entries[1].name, std::string("BETA"));

    // Duplicates stay in order; the count is "extra occurrences".
    const fs::path dup = root / "dup.sog";
    CHECK(write_text(dup, "A\r\nB\r\nA\r\nA\r\na\r\n"));
    gm::audio::Playlist dup_list;
    CHECK(gm::audio::load_playlist(dup, dup_list, &error));
    CHECK_EQ(dup_list.entries.size(), std::size_t{5});
    CHECK_EQ(dup_list.duplicate_count(), 3); // A x2, a x1 (folded)
    printf("duplicates: 5 lines, A B A A a -> %d duplicate(s)\n",
           dup_list.duplicate_count());

    // 8 bytes (the BIGFILE field) loads but can never resolve; 255 loads.
    const fs::path names = root / "names.sog";
    CHECK(write_text(names, boundary_names()));
    gm::audio::Playlist long_list;
    CHECK(gm::audio::load_playlist(names, long_list, &error));
    CHECK_EQ(long_list.entries.size(), std::size_t{2});
    CHECK_EQ(long_list.entries[0].name.size(), gm::audio::kArchiveNameBytes);
    CHECK_EQ(long_list.entries[1].name.size(), gm::audio::kMaxNameBytes);
    CHECK_EQ(long_list.missing_count(), 2);
    printf("name lengths: accepted %zu (8) and %zu (255) bytes\n",
           long_list.entries[0].name.size(), long_list.entries[1].name.size());
}

void test_negatives(const fs::path& root) {
    std::string error;

    gm::audio::Playlist seeded;
    seeded.entries.resize(1);
    seeded.entries[0].name = "KEEP";

    // Missing file.
    const fs::path missing = root / "does-not-exist.sog";
    CHECK(!gm::audio::load_playlist(missing, seeded, &error));
    CHECK(!error.empty());
    CHECK_EQ(seeded.entries.size(), std::size_t{1});
    CHECK_EQ(seeded.entries[0].name, std::string("KEEP"));
    printf("negative missing file: \"%s\"\n", error.c_str());

    // Empty and all-blank files.
    for (const auto& [leaf, text] : std::array<std::pair<const char*, const char*>, 2>{
             {{"empty.sog", ""}, {"blank.sog", " \r\n\r\n\t\r\n"}}}) {
        const fs::path path = root / leaf;
        CHECK(write_text(path, text));
        error.clear();
        seeded.entries[0].name = "KEEP";
        CHECK(!gm::audio::load_playlist(path, seeded, &error));
        CHECK(error.find("empty") != std::string::npos);
        CHECK_EQ(seeded.entries[0].name, std::string("KEEP"));
        printf("negative %s: \"%s\"\n", leaf, error.c_str());
    }

    // A name over the 255-byte limit.
    const fs::path too_long = root / "too-long.sog";
    CHECK(write_text(too_long, repeat('X', gm::audio::kMaxNameBytes + 1) + "\r\n"));
    error.clear();
    CHECK(!gm::audio::load_playlist(too_long, seeded, &error));
    CHECK(error.find("255") != std::string::npos);
    printf("negative name>255: \"%s\"\n", error.c_str());

    // Not a `.sog` path.
    const fs::path other = root / "song.txt";
    CHECK(write_text(other, "A\r\n"));
    error.clear();
    CHECK(!gm::audio::load_playlist(other, seeded, &error));
    CHECK(error.find(".sog") != std::string::npos);
    printf("negative extension: \"%s\"\n", error.c_str());

    // A `.sog` that is a directory.
    const fs::path dir_sog = root / "dir.sog";
    std::error_code ec;
    fs::create_directories(dir_sog, ec);
    error.clear();
    CHECK(!gm::audio::load_playlist(dir_sog, seeded, &error));
    CHECK(!error.empty());

    // An archive with no entries is not "everything missing".
    const fs::path one = root / "one.sog";
    CHECK(write_text(one, "DM1A01\r\n"));
    gm::audio::Playlist playlist;
    CHECK(gm::audio::load_playlist(one, playlist, &error));
    CHECK_EQ(playlist.entries.size(), std::size_t{1});
    const gm::BigFile empty_archive;
    error.clear();
    CHECK(!gm::audio::resolve_playlist(playlist, empty_archive, &error));
    CHECK(!error.empty());
    CHECK(!playlist.resolved);
    CHECK_EQ(playlist.entries[0].index_in_music, -1);
    CHECK_EQ(playlist.found_count(), 0);
    printf("negative empty archive: \"%s\"\n", error.c_str());

    // A missing MUSIC/SOU.IDX: `BigFile::open` refuses with a text.
    error.clear();
    const auto archive = gm::BigFile::open(root / "MUSIC" / "SOU.IDX", error);
    CHECK(!archive.has_value());
    CHECK(!error.empty());
    printf("negative missing MUSIC/SOU.IDX: \"%s\"\n", error.c_str());

    // A directory that does not exist lists nothing (not an error).
    for (const fs::path& absent :
         {root / "nowhere", root / "no-music-dir"}) {
        CHECK(gm::audio::playlist_files(absent).empty());
    }
    // An existing songs-less directory lists nothing either (the `.sog` files
    // of this test live in `root`, so use a clean subdirectory).
    const fs::path no_songs = root / "no-songs";
    fs::create_directories(no_songs, ec);
    CHECK(write_text(no_songs / "readme.txt", "not a playlist\r\n"));
    CHECK(gm::audio::playlist_files(no_songs).empty());
    // The extension test is case-insensitive in the listing too.
    const fs::path mixed_dir = root / "mixed-case";
    fs::create_directories(mixed_dir, ec);
    CHECK(write_text(mixed_dir / "Up.SOG", "A\r\n"));
    CHECK(write_text(mixed_dir / "down.sog", "A\r\n"));
    const auto mixed_list = gm::audio::playlist_files(mixed_dir);
    CHECK_EQ(mixed_list.size(), std::size_t{2});
    CHECK_EQ(mixed_list[0].filename().string(), std::string("Up.SOG"));
    CHECK_EQ(mixed_list[1].filename().string(), std::string("down.sog"));
}

// --------------------------------------------------------------- corpus ----

void test_corpus() {
    const auto game = game_dir();
    if (!game) {
        std::printf("playlist corpus: SKIP (game not installed)\n");
        return;
    }

    std::string error;
    auto archive = gm::BigFile::open(*game / "MUSIC" / "SOU.IDX", error);
    if (!archive) {
        std::printf("playlist corpus: SKIP (MUSIC/SOU.IDX: %s)\n", error.c_str());
        return;
    }
    CHECK_EQ(archive->entries().size(), std::size_t{3092});
    std::printf("archive: MUSIC/SOU.IDX has %zu entries\n", archive->entries().size());

    const auto files = gm::audio::playlist_files(*game);
    CHECK_EQ(files.size(), std::size_t{651});
    std::size_t lower = 0;
    std::size_t upper = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string name = files[i].filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sog") == 0) {
            ++lower;
        } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".SOG") == 0) {
            ++upper;
        }
        if (i > 0) {
            CHECK(files[i - 1].filename().string() < name);
        }
    }
    CHECK_EQ(lower, std::size_t{598});
    CHECK_EQ(upper, std::size_t{53});
    CHECK_EQ(lower + upper, files.size());
    CHECK_EQ(files.front().filename().string(), std::string("Dm1A.sog"));
    CHECK_EQ(files[1].filename().string(), std::string("Dm1B.sog"));
    CHECK_EQ(files[2].filename().string(), std::string("Dm1C.sog"));
    CHECK_EQ(files[files.size() - 3].filename().string(), std::string("s01q.sog"));
    CHECK_EQ(files[files.size() - 2].filename().string(), std::string("s01r.sog"));
    CHECK_EQ(files.back().filename().string(), std::string("winner.sog"));
    std::printf("files: %zu (%zu .sog + %zu .SOG) first=%s,%s,%s last=%s,%s,%s\n",
                files.size(), lower, upper, files[0].filename().string().c_str(),
                files[1].filename().string().c_str(), files[2].filename().string().c_str(),
                files[files.size() - 3].filename().string().c_str(),
                files[files.size() - 2].filename().string().c_str(),
                files.back().filename().string().c_str());

    // The songs-directory overload names the same 651 files.
    const auto via_music = gm::audio::playlist_files(*game / "MUSIC");
    CHECK_EQ(via_music.size(), files.size());
    CHECK(std::equal(files.begin(), files.end(), via_music.begin(),
                     [](const fs::path& a, const fs::path& b) {
                         return a.filename() == b.filename();
                     }));

    // ---- five real playlists -------------------------------------------
    struct Row {
        const char* file;
        int names;
        int found;
        int missing;
        int duplicates;
        std::int64_t bytes;
        std::array<const char*, 3> first_name;
        std::array<int, 3> first_index;
        std::array<int, 3> first_bytes;
    };
    const std::array<Row, 5> rows{{
        {"Dm1A.sog", 8, 8, 0, 0, 244204,
         {{"Dm1A01", "Dm1A02", "Dm1A03"}}, {{0, 1, 2}}, {{31148, 23716, 31876}}},
        {"Dm1B.sog", 8, 7, 1, 0, 236876,
         {{"Dm1B01", "Dm1B02", "Dm1B03"}}, {{8, 9, 10}}, {{33312, 34272, 34404}}},
        {"L26I.sog", 112, 112, 0, 72, 2116524,
         {{"L26J01", "L26J02", "L26J03"}}, {{2732, 2733, 2734}}, {{18988, 18904, 19060}}},
        {"Dm2A.SOG", 32, 32, 0, 28, 648652,
         {{"Dm2A01", "Dm2A02", "Dm2A03"}}, {{73, 74, 75}}, {{19520, 19532, 20852}}},
        {"winner.sog", 1, 1, 0, 0, 1341632, {{"winner", "", ""}}, {{3091, 0, 0}},
         {{1341632, 0, 0}}},
    }};

    for (const Row& row : rows) {
        gm::audio::Playlist playlist;
        error.clear();
        if (!gm::audio::load_playlist(*game / "MUSIC" / row.file, playlist, &error)) {
            std::printf("playlist %s: load failed: %s\n", row.file, error.c_str());
            CHECK(false);
            continue;
        }
        CHECK(gm::audio::resolve_playlist(playlist, *archive, &error));
        fprintf(stdout, "%s: %zu names found=%d missing=%d dups=%d bytes=%lld\n",
                row.file, playlist.entries.size(), playlist.found_count(),
                playlist.missing_count(), playlist.duplicate_count(),
                static_cast<long long>(playlist.found_bytes()));
        CHECK_EQ(static_cast<int>(playlist.entries.size()), row.names);
        CHECK_EQ(playlist.found_count(), row.found);
        CHECK_EQ(playlist.missing_count(), row.missing);
        CHECK_EQ(playlist.duplicate_count(), row.duplicates);
        CHECK_EQ(playlist.found_bytes(), row.bytes);
        CHECK(playlist.resolved);

        const std::size_t pinned =
            std::min<std::size_t>(3, playlist.entries.size());
        for (std::size_t i = 0; i < pinned; ++i) {
            const gm::audio::PlaylistEntry& entry = playlist.entries[i];
            fprintf(stdout, "  [%zu] %s -> index=%d bytes=%d found=%d\n", i,
                    entry.name.c_str(), entry.index_in_music, entry.bytes,
                    entry.found ? 1 : 0);
            CHECK_EQ(entry.name, std::string(row.first_name[i]));
            CHECK_EQ(entry.index_in_music, row.first_index[i]);
            CHECK_EQ(entry.bytes, row.first_bytes[i]);
            CHECK(entry.found);
        }
    }

    // Dm1B.sog's last line is the one shipped miss of the file.
    {
        gm::audio::Playlist playlist;
        CHECK(gm::audio::load_playlist(*game / "MUSIC" / "Dm1B.sog", playlist, &error));
        CHECK(gm::audio::resolve_playlist(playlist, *archive, &error));
        CHECK_EQ(playlist.entries.back().name, std::string("Dm1C04"));
        CHECK(!playlist.entries.back().found);
        CHECK_EQ(playlist.entries.back().index_in_music, -1);
        CHECK_EQ(playlist.entries.back().bytes, 0);
        const auto missing = playlist.missing_names();
        CHECK_EQ(missing.size(), std::size_t{1});
        CHECK_EQ(missing[0], std::string("DM1C04"));
        std::printf("Dm1B.sog miss: \"%s\" (line 8)\n", missing[0].c_str());
    }

    // ---- the whole corpus ----------------------------------------------
    std::set<std::string> exact_names;
    for (const gm::BigEntry& entry : archive->entries()) {
        exact_names.insert(entry.name);
    }

    int names = 0;
    int found = 0;
    int missing = 0;
    int duplicates = 0;
    int duplicate_playlists = 0;
    int missing_playlists = 0;
    int case_sensitive_found = 0;
    int mixed_case = 0;
    std::int64_t bytes = 0;
    std::set<std::string> used_folded;
    std::map<std::string, int> absent; // uppercase name -> occurrences
    for (const fs::path& file : files) {
        gm::audio::Playlist playlist;
        error.clear();
        if (!gm::audio::load_playlist(file, playlist, &error)) {
            std::printf("corpus load failed: %s: %s\n", file.filename().string().c_str(),
                        error.c_str());
            CHECK(false);
            continue;
        }
        if (!gm::audio::resolve_playlist(playlist, *archive, &error)) {
            std::printf("corpus resolve failed: %s\n", error.c_str());
            CHECK(false);
            continue;
        }
        names += static_cast<int>(playlist.entries.size());
        found += playlist.found_count();
        missing += playlist.missing_count();
        bytes += playlist.found_bytes();
        duplicates += playlist.duplicate_count();
        if (playlist.duplicate_count() > 0) {
            ++duplicate_playlists;
        }
        if (playlist.missing_count() > 0) {
            ++missing_playlists;
        }
        for (const gm::audio::PlaylistEntry& entry : playlist.entries) {
            if (exact_names.count(entry.name) != 0) {
                ++case_sensitive_found;
            }
            if (upper_of(entry.name) != entry.name) {
                ++mixed_case;
            }
            used_folded.insert(folded(entry.name));
            if (!entry.found) {
                ++absent[upper_of(entry.name)];
            }
        }
    }

    CHECK_EQ(names, 5211);
    CHECK_EQ(found, 5088);
    CHECK_EQ(missing, 123);
    CHECK_EQ(bytes, std::int64_t{124138728});
    CHECK_EQ(duplicates, 998);
    CHECK_EQ(duplicate_playlists, 131);
    CHECK_EQ(missing_playlists, 25);
    CHECK_EQ(absent.size(), std::size_t{95});
    CHECK_EQ(case_sensitive_found, 4038);
    CHECK_EQ(mixed_case, 1054);
    CHECK_EQ(used_folded.size(), std::size_t{3017});
    std::printf("corpus: names=%d found=%d missing=%d bytes=%lld dups=%d "
                "dup_playlists=%d missing_playlists=%d unique_missing=%zu "
                "case_sensitive_found=%d mixed_case=%d distinct_names=%zu\n",
                names, found, missing, static_cast<long long>(bytes), duplicates,
                duplicate_playlists, missing_playlists, absent.size(),
                case_sensitive_found, mixed_case, used_folded.size());

    // Top five absent names (count desc, then name) — the shipped cut content.
    std::vector<std::pair<std::string, int>> ranked(absent.begin(), absent.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) {
                      return a.second > b.second;
                  }
                  return a.first < b.first;
              });
    const std::array<std::pair<const char*, int>, 5> top{{
        {"L24H12", 4}, {"L10H02", 3}, {"L24N04", 3}, {"DM1C04", 2}, {"L10G08", 2}}};
    CHECK(ranked.size() >= top.size());
    for (std::size_t i = 0; i < top.size() && i < ranked.size(); ++i) {
        CHECK_EQ(ranked[i].first, std::string(top[i].first));
        CHECK_EQ(ranked[i].second, top[i].second);
    }
    std::string top_line;
    for (std::size_t i = 0; i < top.size() && i < ranked.size(); ++i) {
        top_line += (i == 0 ? "" : ", ") + ranked[i].first + "=" +
                    std::to_string(ranked[i].second);
    }
    std::printf("top missing: %s\n", top_line.c_str());

    // ---- determinism ---------------------------------------------------
    {
        gm::audio::Playlist first;
        gm::audio::Playlist second;
        CHECK(gm::audio::load_playlist(*game / "MUSIC" / "L26I.sog", first, &error));
        CHECK(gm::audio::load_playlist(*game / "MUSIC" / "L26I.sog", second, &error));
        CHECK(gm::audio::resolve_playlist(first, *archive, &error));
        CHECK(gm::audio::resolve_playlist(second, *archive, &error));
        CHECK_EQ(first.entries.size(), second.entries.size());
        bool same = first.entries.size() == second.entries.size();
        for (std::size_t i = 0; same && i < first.entries.size(); ++i) {
            same = first.entries[i].name == second.entries[i].name &&
                   first.entries[i].index_in_music == second.entries[i].index_in_music &&
                   first.entries[i].bytes == second.entries[i].bytes &&
                   first.entries[i].found == second.entries[i].found;
        }
        CHECK(same);

        // Re-resolving the same playlist is idempotent.
        CHECK(gm::audio::resolve_playlist(first, *archive, &error));
        CHECK(first.found_count() == second.found_count());
        CHECK(first.found_bytes() == second.found_bytes());
        std::printf("determinism: L26I.sog 2 loads + 3 resolves agree "
                    "(found=%d bytes=%lld)\n",
                    first.found_count(), static_cast<long long>(first.found_bytes()));
    }
}

} // namespace

int main() {
    const fs::path root = temp_root();
    test_line_model(root);
    test_negatives(root);
    test_corpus();

    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_failures == 0) {
        std::printf("playlist: %d checks, ok\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "playlist: %d failure(s) of %d checks\n", g_failures, g_checks);
    return 1;
}
