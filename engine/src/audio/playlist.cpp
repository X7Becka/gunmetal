#include "audio/playlist.h"

#include "res/text.h" // gm::parse_sog, the CLI's line model

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace gm::audio {
namespace {

namespace fs = std::filesystem;

/// ASCII-only fold.  Bytes >= 0x80 are left alone (latin-1 identity), so a
/// locale cannot change a name: every shipped name is ASCII.
std::string fold_ascii(std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

std::string upper_ascii(std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return key;
}

/// Lower-cased extension, e.g. `.sog` for `Dm2A.SOG`.
std::string extension_lower(const fs::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

bool blank_bytes(std::string_view data) {
    return std::all_of(data.begin(), data.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
}

void fail(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

} // namespace

// ------------------------------------------------------------- counters ----

int Playlist::found_count() const {
    return static_cast<int>(std::count_if(
        entries.begin(), entries.end(), [](const PlaylistEntry& e) { return e.found; }));
}

int Playlist::missing_count() const {
    return static_cast<int>(entries.size()) - found_count();
}

int Playlist::duplicate_count() const {
    std::map<std::string, int> seen;
    int extra = 0;
    for (const PlaylistEntry& entry : entries) {
        if (++seen[fold_ascii(entry.name)] > 1) {
            ++extra;
        }
    }
    return extra;
}

std::int64_t Playlist::found_bytes() const {
    std::int64_t total = 0;
    for (const PlaylistEntry& entry : entries) {
        if (entry.found) {
            total += entry.bytes;
        }
    }
    return total;
}

std::vector<std::string> Playlist::missing_names() const {
    std::set<std::string> names;
    for (const PlaylistEntry& entry : entries) {
        if (!entry.found) {
            names.insert(upper_ascii(entry.name));
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

// ---------------------------------------------------------------- load ----

bool load_playlist(const fs::path& file, Playlist& out, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    if (extension_lower(file) != ".sog") {
        fail(error, "not a .sog file: " + file.filename().string());
        return false;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        fail(error, "cannot open " + file.string());
        return false;
    }
    const std::string data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (blank_bytes(data)) {
        fail(error, "empty playlist: " + file.filename().string());
        return false;
    }

    std::string parse_error;
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    const auto parsed = gm::parse_sog(bytes, parse_error);
    if (!parsed) {
        fail(error, file.filename().string() + ": " + parse_error);
        return false;
    }

    Playlist result;
    result.path = file;
    result.entries.reserve(parsed->names.size());
    for (const std::string& name : parsed->names) {
        if (name.size() > kMaxNameBytes) {
            fail(error, "sample name longer than " + std::to_string(kMaxNameBytes) +
                            " bytes in " + file.filename().string());
            return false;
        }
        PlaylistEntry entry;
        entry.name = name;
        result.entries.push_back(std::move(entry));
    }
    out = std::move(result);
    return true;
}

// ------------------------------------------------------------- resolve ----

bool resolve_playlist(Playlist& playlist, const BigFile& music, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    const std::vector<BigEntry>& archive = music.entries();
    if (archive.empty()) {
        fail(error, "music archive has no entries");
        return false;
    }

    // Folded name -> first entry index.  `emplace` keeps the first occurrence,
    // so a crafted archive with two identical names still resolves to the
    // lowest index (the shipped archive has 3092 unique names).
    std::map<std::string, int> index;
    for (std::size_t i = 0; i < archive.size(); ++i) {
        index.emplace(fold_ascii(archive[i].name), static_cast<int>(i));
    }

    for (PlaylistEntry& entry : playlist.entries) {
        const auto it = index.find(fold_ascii(entry.name));
        if (it == index.end()) {
            entry.index_in_music = -1;
            entry.bytes = 0;
            entry.found = false;
            continue;
        }
        const BigEntry& hit = archive[static_cast<std::size_t>(it->second)];
        entry.index_in_music = it->second;
        entry.bytes = static_cast<int>(std::min<std::uint64_t>(
            hit.size, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        entry.found = true;
    }
    playlist.resolved = true;
    return true;
}

// --------------------------------------------------------------- listing ----

std::vector<fs::path> playlist_files(const fs::path& game) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path dir = game / "MUSIC";
    if (!fs::is_directory(dir, ec)) {
        dir = game; // a bare songs directory is accepted too
    }
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (extension_lower(entry.path()) != ".sog") {
            continue;
        }
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return out;
}

} // namespace gm::audio
