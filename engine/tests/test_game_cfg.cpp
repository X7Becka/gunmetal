// `GUNMETAL.CFG` reader/writer (`M12.5`/`M1.5`, slice 1; docs/GAME_CFG.md).
//
// Pins `game/game_cfg.h` against the shipped `GUNMETAL.CFG` (read only: every
// write goes to a scratch file under /var/tmp) and against the facts of
// `docs/CONFIG.md` §2/§3:
//
//   * the shipped image is 8192 (0x2000) bytes with FNV-1a 64
//     0x439e3e13a3242648, magic `"GUNMETAL Configuration File. av1.93"`, byte
//     0x23 = 0x1A and byte 0x24 = 0x00;
//   * the sixteen proven fields read 16, 20, 0, 800, 600, 8, 3, 1, 1, 2, 1,
//     0x10400, 0x10400, 0, 7, 4 -- exactly the values of `docs/CONFIG.md` §3,
//     and each equals the little-endian dword at its offset in `raw`;
//   * `game_cfg_bytes()` of the loaded image is byte-identical to the file
//     (8192/8192 bytes, 0 differing bytes), and a save -> read -> load cycle
//     reproduces the same bytes;
//   * `set_field()` changes only the four bytes of its own field (the diff
//     against the original is counted and located), leaves `raw` untouched,
//     and for `play_flags_p1` it does not touch the `play_flags_p2` word;
//   * the range boundaries of every settable field (gamma 0..3, border 0..9,
//     light maps 0..2, dynamic camera 0..2, DM type 0..2, starting cash 0..11,
//     reflections/left-right 0/1, volumes 0..32, play bits 0x11D400, DM flags
//     0x75) accept the ends and refuse one past them;
//   * read-only fields (`width`, `height`, `colour_depth`), unproven names
//     (`shadows`, `marker`, `unknown_1bc`, `refresh`, `mode_flags`) and an
//     unknown name are refused with a text and leave the field alone;
//   * negatives: 0/8191/8193/16384-byte files, a broken magic byte, the wrong
//     terminator, a missing NUL, an all-zero image -- all refused with a text;
//     a refusal leaves the previously loaded image untouched;
//   * `save_game_cfg()` refuses the installation
//     (`/mnt/d/games/Gunmetal 3dfx`, `GUNMETAL_DIR`, the regress mirror) before
//     opening anything, and the shipped file stays byte-identical;
//   * `CONTROLS.CFG` is 8192 bytes with the 1.91 magic and is refused by the
//     1.93 loader (docs/CONFIG.md §8): the opaque-body fact, not a parse;
//   * two loads/encodes/saves of the same bytes give the same numbers.
//
// When the installation is absent the same checks run on a synthetic
// 8192-byte image built from the shipped values, so the negative and boundary
// halves still guard the module.  The numbers printed at the end are quoted in
// docs/GAME_CFG.md §4.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/bytes.h"
#include "game/game_cfg.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;
int checks = 0;
bool g_shipped_ok = false;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string in_quotes(std::string_view value) { return "\"" + std::string(value) + "\""; }

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

/// FNV-1a 64 of a byte string, the project's usual fingerprint.
std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// A scratch path with a per-process suffix.  `/var/tmp` is the project's
/// scratch disk; `GM_GAME_CFG_TMPDIR` overrides it, the system temp dir is the
/// last resort.
fs::path temp_file(const std::string& leaf) {
    std::string base = "/var/tmp";
    if (const char* env = std::getenv("GM_GAME_CFG_TMPDIR"); env != nullptr && *env != '\0') {
        base = env;
    }
    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        base = fs::temp_directory_path(ec).string();
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&checks)));
    return fs::path(base) / ("gm_game_cfg" + std::string(suffix) + "_" + leaf);
}

/// The installation that carries `GUNMETAL.CFG`; `GUNMETAL_DIR` first, then the
/// shipped path, then the `regress.py` mirror.
fs::path shipped_dir() {
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (fs::is_regular_file(root / "GUNMETAL.CFG", ec) && !ec) {
            return root;
        }
    }
    return {};
}

/// The value of a field by image offset.  Deliberately independent of the
/// implementation's own offset -> member switch: this is the mapping check.
int field_of(const gm::GameCfg& cfg, std::uint32_t offset) {
    switch (offset) {
    case 0x190: return cfg.music_volume;
    case 0x194: return cfg.sfx_volume;
    case 0x19C: return cfg.swap_left_right;
    case 0x1A0: return cfg.width;
    case 0x1A4: return cfg.height;
    case 0x1A8: return cfg.colour_depth;
    case 0x1B4: return cfg.border;
    case 0x1B8: return cfg.gamma;
    case 0x1C0: return cfg.reflections;
    case 0x1C4: return cfg.light_maps;
    case 0x1C8: return cfg.dynamic_camera;
    case 0x1CC: return cfg.play_flags_p1;
    case 0x1D0: return cfg.play_flags_p2;
    case 0x1D4: return cfg.deathmatch_type;
    case 0x1D8: return cfg.starting_cash;
    case 0x1DC: return cfg.deathmatch_flags;
    default: return 0;
    }
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

std::uint32_t get32(std::span<const std::uint8_t> bytes, std::uint32_t offset) {
    return gm::rd32(bytes.data() + offset);
}

/// The shipped field values of `docs/CONFIG.md` §3.
struct ShippedField {
    std::uint32_t offset;
    int value;
};

constexpr ShippedField kShippedValues[] = {
    {0x190, 16}, {0x194, 20}, {0x19C, 0}, {0x1A0, 800}, {0x1A4, 600}, {0x1A8, 8},
    {0x1B4, 3},  {0x1B8, 1},  {0x1C0, 1}, {0x1C4, 2},   {0x1C8, 1},   {0x1CC, 0x10400},
    {0x1D0, 0x10400}, {0x1D4, 0}, {0x1D8, 7}, {0x1DC, 4},
};

/// A valid 8192-byte image for machines without the installation: the shipped
/// magic, terminator and field values, plus a few non-field bytes that must
/// survive a round-trip.
std::vector<std::uint8_t> synthetic_image() {
    std::vector<std::uint8_t> bytes(gm::kGameCfgSize, 0);
    std::memcpy(bytes.data(), gm::kGameCfgMagic.data(), gm::kGameCfgMagic.size());
    bytes[gm::kGameCfgMagic.size()] = gm::kGameCfgTerminator;
    bytes[gm::kGameCfgMagic.size() + 1] = 0x00;
    for (const ShippedField& field : kShippedValues) {
        put32(bytes, field.offset, static_cast<std::uint32_t>(field.value));
    }
    // Opaque gaps and blocks of docs/CONFIG.md §2/§4/§6: not fields, preserved.
    bytes[0x100] = 0xA5;
    bytes[0x18F] = 0x5A;
    bytes[0x8AB] = 0xC3;
    bytes[0x1FFF] = 0x3C;
    std::memcpy(bytes.data() + 0x1E0, "PERFECT\0", 8);
    std::memcpy(bytes.data() + 0x1E4, "SPLIT1\0", 7);
    return bytes;
}

/// The loaded base image: shipped when installed, synthetic otherwise.
struct Base {
    gm::GameCfg cfg;
    std::vector<std::uint8_t> bytes;
    fs::path path;
    bool shipped = false;
};

Base load_base() {
    Base base;
    base.path = shipped_dir();
    if (!base.path.empty()) {
        base.path /= "GUNMETAL.CFG";
        base.shipped = read_file(base.path, base.bytes);
    }
    if (!base.shipped) {
        base.path.clear();
        base.bytes = synthetic_image();
    }
    std::string error;
    const bool ok = base.shipped ? gm::load_game_cfg(base.path, base.cfg, &error)
                                 : gm::parse_game_cfg(base.bytes, base.cfg, &error);
    g_shipped_ok = g_shipped_ok || base.shipped;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: the base image does not load: %s\n", error.c_str());
    }
    return base;
}

/// True when every field of `a` and `b` has the same value.
bool same_fields(const gm::GameCfg& a, const gm::GameCfg& b) {
    for (const gm::GameCfgField& field : gm::game_cfg_fields()) {
        if (field_of(a, field.offset) != field_of(b, field.offset)) {
            return false;
        }
    }
    return true;
}

/// The number of differing bytes in the four bytes of an old/new dword value.
std::size_t byte_diff(int old_value, int new_value) {
    const auto old_bits = static_cast<std::uint32_t>(old_value);
    const auto new_bits = static_cast<std::uint32_t>(new_value);
    std::size_t count = 0;
    for (int i = 0; i < 4; ++i) {
        if (((old_bits >> (8 * i)) & 0xFF) != ((new_bits >> (8 * i)) & 0xFF)) {
            ++count;
        }
    }
    return count;
}

/// Byte offsets that differ between two 8192-byte images, with their count.
struct Diff {
    std::size_t count = 0;
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    bool all_in(std::uint32_t offset) const {
        return count == 0 || (first >= offset && last < offset + 4);
    }
};

Diff diff_bytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    Diff diff;
    if (a.size() != b.size()) {
        diff.count = a.size() > b.size() ? a.size() : b.size();
        diff.first = 0;
        diff.last = static_cast<std::uint32_t>(diff.count == 0 ? 0 : diff.count - 1);
        return diff;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            if (diff.count == 0) {
                diff.first = static_cast<std::uint32_t>(i);
            }
            diff.last = static_cast<std::uint32_t>(i);
            ++diff.count;
        }
    }
    return diff;
}

void test_shipped(const Base& base) {
    check(base.cfg.raw.size() == gm::kGameCfgSize, "raw is 8192 bytes");
    check(base.bytes.size() == gm::kGameCfgSize, "the base image is 8192 bytes");
    check(base.cfg.header == gm::kGameCfgMagic, "the header is the 1.93 magic");
    check(base.cfg.header.size() == 35, "the header text is 35 bytes");
    check(gm::game_cfg_version(base.cfg) == "1.93", "the version is 1.93");
    check(base.bytes[0x23] == 0x1A, "byte 0x23 is 0x1a");
    check(base.bytes[0x24] == 0x00, "byte 0x24 is the NUL terminator");
    check(base.cfg.raw == base.bytes, "raw is the file byte for byte");

    if (base.shipped) {
        const std::uint64_t hash = fnv1a64(base.bytes);
        check(hash == 0x439e3e13a3242648ull, "the shipped FNV-1a 64 is 439e3e13a3242648");
    }

    // The documented values, both in the struct and in raw.
    for (const ShippedField& field : kShippedValues) {
        check(field_of(base.cfg, field.offset) == field.value,
              "field at 0x" + std::to_string(field.offset) + " has the shipped value " +
                  std::to_string(field.value));
        check(static_cast<int>(get32(base.cfg.raw, field.offset)) == field.value,
              "raw dword at 0x" + std::to_string(field.offset) + " has the shipped value");
    }

    // Every table entry points at its own struct member and at its raw dword.
    for (const gm::GameCfgField& field : gm::game_cfg_fields()) {
        check(field_of(base.cfg, field.offset) == static_cast<int>(get32(base.cfg.raw, field.offset)),
              std::string(field.name) + ": the member equals raw[0x" +
                  std::to_string(field.offset) + "]");
        const gm::GameCfgField* found = gm::find_game_cfg_field(field.name);
        check(found != nullptr && found->offset == field.offset,
              std::string(field.name) + ": find_game_cfg_field() finds the same entry");
    }

    // Loaded -> encoded must be byte-exact, and a save/reload must keep it.
    std::vector<std::uint8_t> encoded;
    std::string error;
    check(gm::game_cfg_bytes(base.cfg, encoded, &error), "game_cfg_bytes() succeeds");
    check(encoded.size() == gm::kGameCfgSize, "the encoded image is 8192 bytes");
    const Diff round_trip = diff_bytes(base.bytes, encoded);
    check(round_trip.count == 0, "the encoded image has 0 differing bytes");
    check(fnv1a64(encoded) == fnv1a64(base.bytes), "the encoded FNV matches the source");

    const fs::path scratch = temp_file("roundtrip.cfg");
    check(gm::save_game_cfg(scratch, base.cfg, &error), "save_game_cfg() to a scratch copy: " + error);
    std::vector<std::uint8_t> written;
    check(read_file(scratch, written), "the scratch copy is readable");
    check(written.size() == 8192, "the scratch copy is 8192 bytes");
    check(diff_bytes(base.bytes, written).count == 0, "round-trip is byte for byte (8192/8192)");

    gm::GameCfg reloaded;
    check(gm::load_game_cfg(scratch, reloaded, &error), "the scratch copy loads: " + error);
    check(reloaded.raw == base.cfg.raw, "the reloaded raw image is identical");
    check(same_fields(reloaded, base.cfg), "the reloaded fields are identical");
    std::error_code ec;
    fs::remove(scratch, ec);

    std::printf("game_cfg: GUNMETAL.CFG 8192 B fnv=%016llx fields=%d round-trip=%zu/%zu "
                "music=%d effects=%d %dx%dx%d border=%d gamma=%d light_maps=%d camera=%d "
                "play=0x%x cash=%d dm_flags=0x%x\n",
                static_cast<unsigned long long>(fnv1a64(base.bytes)),
                static_cast<int>(gm::game_cfg_fields().size()), 8192 - round_trip.count,
                static_cast<std::size_t>(8192), base.cfg.music_volume, base.cfg.sfx_volume,
                base.cfg.width, base.cfg.height, base.cfg.colour_depth, base.cfg.border,
                base.cfg.gamma, base.cfg.light_maps, base.cfg.dynamic_camera,
                base.cfg.play_flags_p1, base.cfg.starting_cash, base.cfg.deathmatch_flags);
}

void test_field_table() {
    const std::vector<gm::GameCfgField>& fields = gm::game_cfg_fields();
    check(fields.size() == 16, "the table has 16 proven fields");
    check(gm::kPlayAllOptions == (gm::kPlaySkipEmptyWeapons | gm::kPlayDontAutoTarget |
                                  gm::kPlaySkipDeadlyWeapons | gm::kPlaySwitchToPickedUpAmmo |
                                  gm::kPlayAutoSwitchDepleted | gm::kPlayMouseCheating),
          "the six play bits OR to 0x11D400");
    check(gm::kPlayAllOptions == 0x11D400, "kPlayAllOptions is 0x11d400");
    check(gm::kDmAllFlags == 0x75, "the DM flag mask is 0x75");

    int settable = 0;
    std::uint32_t previous = 0;
    std::string names;
    for (const gm::GameCfgField& field : fields) {
        if (!names.empty()) {
            names += ',';
        }
        names += field.name;
        settable += field.settable ? 1 : 0;
        check(field.offset >= 0x190 && field.offset <= 0x1DC, std::string(field.name) +
                  ": offset is in 0x190..0x1DC");
        check((field.offset - 0x190) % 4 == 0, std::string(field.name) + ": offset is 4-aligned");
        check(field.offset > previous, "the table is in ascending offset order");
        previous = field.offset;
        check(!field.source.empty(), std::string(field.name) + ": carries its provenance");
    }
    check(settable == 13, "13 fields are settable");
    check(fields.size() - static_cast<std::size_t>(settable) == 3, "3 fields are read-only");
    check(names == "music_volume,sfx_volume,swap_left_right,width,height,colour_depth,border,"
                   "gamma,reflections,light_maps,dynamic_camera,play_flags_p1,play_flags_p2,"
                   "deathmatch_type,starting_cash,deathmatch_flags",
          "only the proven fields are exposed: " + names);

    // Not proven -> not a field (docs/CONFIG.md §11 hypothesis list).
    for (const char* name : {"shadows", "marker", "unknown_1bc", "refresh", "mode_flags",
                             "magic", "Music_Volume", ""}) {
        check(gm::find_game_cfg_field(name) == nullptr,
              std::string("no field named ") + in_quotes(name));
    }
    check(gm::find_game_cfg_field("music_volume") != nullptr, "music_volume is a field");
    std::printf("game_cfg: table fields=%d settable=%d read_only=%d names=%s\n",
                static_cast<int>(fields.size()), settable,
                static_cast<int>(fields.size()) - settable, names.c_str());
}

void test_set_field(const Base& base) {
    int settable = 0;
    std::size_t total_bytes = 0;
    std::size_t max_bytes = 0;
    std::string examples;

    for (const gm::GameCfgField& field : gm::game_cfg_fields()) {
        if (!field.settable) {
            continue;
        }
        ++settable;
        const int old_value = field_of(base.cfg, field.offset);
        int new_value = field.min_value;
        if (field.allowed_mask != 0) {
            new_value = (old_value == static_cast<int>(field.allowed_mask))
                            ? field.min_value
                            : static_cast<int>(field.allowed_mask);
        } else if (old_value == field.min_value) {
            new_value = field.max_value;
        }

        gm::GameCfg cfg = base.cfg;
        std::string error;
        const std::string name(field.name);
        check(gm::set_field(cfg, name, new_value, &error),
              name + ": set_field(" + std::to_string(new_value) + ") succeeds: " + error);
        check(error.empty(), name + ": success clears the error text");
        check(field_of(cfg, field.offset) == new_value, name + ": the member takes the new value");
        check(cfg.raw == base.cfg.raw, name + ": set_field leaves raw untouched");

        bool others_same = true;
        for (const gm::GameCfgField& other : gm::game_cfg_fields()) {
            if (other.offset != field.offset &&
                field_of(cfg, other.offset) != field_of(base.cfg, other.offset)) {
                others_same = false;
            }
        }
        check(others_same, name + ": no other field changed");

        std::vector<std::uint8_t> encoded;
        check(gm::game_cfg_bytes(cfg, encoded, &error), name + ": game_cfg_bytes() succeeds");
        const Diff diff = diff_bytes(base.bytes, encoded);
        const std::size_t expected = byte_diff(old_value, new_value);
        check(diff.all_in(field.offset),
              name + ": every changed byte is in 0x" + std::to_string(field.offset) + "..0x" +
                  std::to_string(field.offset + 3) + " (first 0x" + std::to_string(diff.first) +
                  ", last 0x" + std::to_string(diff.last) + ")");
        check(diff.count == expected,
              name + ": " + std::to_string(diff.count) + " changed bytes, expected " +
                  std::to_string(expected));
        check(static_cast<int>(get32(encoded, field.offset)) == new_value,
              name + ": the encoded dword is the new value");
        total_bytes += diff.count;
        if (diff.count > max_bytes) {
            max_bytes = diff.count;
        }
        if (examples.size() < 400) {
            examples += (examples.empty() ? "" : " ") + name + "=" + std::to_string(old_value) +
                        "->" + std::to_string(new_value) + ":" + std::to_string(diff.count) + "B";
        }

        // The same diff must reach the disk and survive a reload.
        const fs::path scratch = temp_file("set_" + name + ".cfg");
        check(gm::save_game_cfg(scratch, cfg, &error), name + ": save succeeds: " + error);
        std::vector<std::uint8_t> written;
        check(read_file(scratch, written), name + ": the saved copy is readable");
        check(diff.count == diff_bytes(base.bytes, written).count,
              name + ": the file differs in the same " + std::to_string(diff.count) + " bytes");
        gm::GameCfg reloaded;
        check(gm::load_game_cfg(scratch, reloaded, &error), name + ": the saved copy reloads");
        check(field_of(reloaded, field.offset) == new_value, name + ": the reloaded value is new");
        bool reload_same = true;
        for (const gm::GameCfgField& other : gm::game_cfg_fields()) {
            if (other.offset != field.offset &&
                field_of(reloaded, other.offset) != field_of(base.cfg, other.offset)) {
                reload_same = false;
            }
        }
        check(reload_same, name + ": the reloaded other fields are unchanged");
        std::error_code ec;
        fs::remove(scratch, ec);
    }
    check(settable == 13, "13 settable fields were exercised");
    std::printf("game_cfg: set_field fields=%d changed_bytes=%zu max_per_field=%zu %s\n", settable,
                total_bytes, max_bytes, examples.c_str());

    // The two copies of the play-option word are independent fields.
    {
        gm::GameCfg cfg = base.cfg;
        std::string error;
        check(gm::set_field(cfg, "play_flags_p1", gm::kPlaySkipDeadlyWeapons, &error),
              "play_flags_p1 = 0x4000 succeeds");
        std::vector<std::uint8_t> encoded;
        check(gm::game_cfg_bytes(cfg, encoded, &error), "the play-flag edit encodes");
        check(get32(encoded, 0x1D0) == get32(base.bytes, 0x1D0),
              "play_flags_p1 = 0x4000 does not touch play_flags_p2 at 0x1d0");
        check(cfg.play_flags_p2 == base.cfg.play_flags_p2, "the p2 member is unchanged");
    }

    // The documented border example: 3 -> 9 is exactly one byte at 0x1b4.
    {
        gm::GameCfg cfg = base.cfg;
        std::string error;
        check(gm::set_field(cfg, "border", 9, &error), "border = 9 succeeds");
        std::vector<std::uint8_t> encoded;
        check(gm::game_cfg_bytes(cfg, encoded, &error), "the border edit encodes");
        const Diff diff = diff_bytes(base.bytes, encoded);
        check(diff.count == 1 && diff.first == 0x1B4,
              "border 3 -> 9 changes exactly one byte at 0x1b4");
    }

    // Boundaries: both ends accepted, one past either end refused.
    for (const gm::GameCfgField& field : gm::game_cfg_fields()) {
        if (!field.settable) {
            continue;
        }
        const std::string name(field.name);
        for (const int value : {field.min_value, field.max_value}) {
            gm::GameCfg cfg = base.cfg;
            std::string error;
            check(gm::set_field(cfg, name, value, &error),
                  name + ": boundary " + std::to_string(value) + " is accepted: " + error);
            check(field_of(cfg, field.offset) == value, name + ": the boundary value is stored");
        }
        for (const int value : {field.min_value - 1, field.max_value + 1}) {
            gm::GameCfg cfg = base.cfg;
            std::string error;
            const int before = field_of(cfg, field.offset);
            check(!gm::set_field(cfg, name, value, &error),
                  name + ": " + std::to_string(value) + " is refused");
            check(contains(error, "GUNMETAL.CFG:") && contains(error, "outside"),
                  name + ": the range text is explicit: " + error);
            check(field_of(cfg, field.offset) == before, name + ": a refused range keeps the value");
        }
    }

    // A negative value is out of every proven range.
    for (const gm::GameCfgField& field : gm::game_cfg_fields()) {
        if (!field.settable) {
            continue;
        }
        gm::GameCfg cfg = base.cfg;
        std::string error;
        check(!gm::set_field(cfg, std::string(field.name), -1, &error),
              std::string(field.name) + ": -1 is refused");
    }

    // Bits outside the proven masks.
    {
        gm::GameCfg cfg = base.cfg;
        std::string error;
        check(!gm::set_field(cfg, "play_flags_p1", 0x1, &error), "play bit 0x1 is refused");
        check(contains(error, "bits outside 0x11d400"), "the play-bit mask is printed: " + error);
        check(cfg.play_flags_p1 == base.cfg.play_flags_p1, "a refused play bit keeps the word");
        check(!gm::set_field(cfg, "play_flags_p2", 0x200000, &error),
              "play bit 0x200000 is refused");
        check(!gm::set_field(cfg, "deathmatch_flags", 0x02, &error),
              "DM flag bit 1 is refused");
        check(contains(error, "bits outside 0x75"), "the DM mask is printed: " + error);
        check(gm::set_field(cfg, "deathmatch_flags", gm::kDmAllFlags, &error),
              "the five DM bits 0x75 are accepted");
        check(gm::set_field(cfg, "play_flags_p1", gm::kPlayAllOptions, &error),
              "all six play bits 0x11d400 are accepted");
    }

    // Read-only fields: the game fills them from the mode table.
    for (const char* name : {"width", "height", "colour_depth"}) {
        gm::GameCfg cfg = base.cfg;
        std::string error;
        const gm::GameCfgField* field = gm::find_game_cfg_field(name);
        check(field != nullptr, std::string("the read-only field ") + name + " exists");
        const int before = field == nullptr ? 0 : field_of(cfg, field->offset);
        check(!gm::set_field(cfg, name, 1024, &error), std::string(name) + ": setting is refused");
        check(contains(error, "read-only"), std::string(name) + ": the text says read-only: " + error);
        check(field != nullptr && field_of(cfg, field->offset) == before,
              std::string(name) + ": the value is unchanged");
    }

    // Unknown/unproven names.
    for (const char* name : {"shadows", "marker", "unknown_1bc", "refresh", "mode_flags", ""}) {
        gm::GameCfg cfg = base.cfg;
        std::string error;
        check(!gm::set_field(cfg, name, 0, &error), std::string("set_field(") + in_quotes(name) +
                  ") is refused");
        check(contains(error, "unknown field"), std::string(name) + ": the text says unknown: " +
                  error);
        check(same_fields(cfg, base.cfg), std::string(name) + ": no field changed");
    }
}

void test_negatives(const Base& base) {
    std::string error;

    // Missing file.
    {
        gm::GameCfg cfg = base.cfg;
        check(!gm::load_game_cfg(temp_file("missing.cfg"), cfg, &error),
              "a missing file is refused");
        check(contains(error, "GUNMETAL.CFG:") && contains(error, "no such file"),
              "the missing-file text is explicit: " + error);
        check(cfg.raw == base.cfg.raw && same_fields(cfg, base.cfg),
              "a missing file leaves the loaded image untouched");
    }

    // Wrong sizes: 0, 8191, 8193, 16384.
    for (const std::size_t size : {std::size_t{0}, std::size_t{8191}, std::size_t{8193},
                                   std::size_t{16384}}) {
        std::vector<std::uint8_t> bytes(base.bytes);
        bytes.resize(size, 0x00);
        const fs::path file = temp_file("size_" + std::to_string(size) + ".cfg");
        check(write_file(file, bytes), "the " + std::to_string(size) + "-byte file is written");
        gm::GameCfg cfg = base.cfg;
        check(!gm::load_game_cfg(file, cfg, &error),
              "a " + std::to_string(size) + "-byte file is refused");
        check(contains(error, std::to_string(size) + " bytes, expected 8192"),
              "the size text is explicit: " + error);
        check(cfg.raw == base.cfg.raw && same_fields(cfg, base.cfg),
              "a " + std::to_string(size) + "-byte file leaves the loaded image untouched");
        std::error_code ec;
        fs::remove(file, ec);
    }

    // Broken headers: a magic byte, the terminator, the NUL, an all-zero image.
    struct HeaderCase {
        const char* name;
        std::uint32_t offset;
        std::uint8_t value;
        const char* text;
    };
    const HeaderCase header_cases[] = {
        {"av1.92", 34, '2', "header"},
        {"first byte", 0, 'X', "header"},
        {"terminator 0x1b", 0x23, 0x1B, "0x1a"},
        {"NUL -> 'A'", 0x24, 'A', "0x24"},
    };
    for (const HeaderCase& item : header_cases) {
        std::vector<std::uint8_t> bytes(base.bytes);
        bytes[item.offset] = item.value;
        const fs::path file = temp_file(std::string("header_") + item.name + ".cfg");
        check(write_file(file, bytes), std::string(item.name) + ": the broken file is written");
        gm::GameCfg cfg = base.cfg;
        check(!gm::load_game_cfg(file, cfg, &error),
              std::string(item.name) + ": the broken header is refused");
        check(contains(error, item.text),
              std::string(item.name) + ": the header text is explicit: " + error);
        check(cfg.raw == base.cfg.raw && same_fields(cfg, base.cfg),
              std::string(item.name) + ": the loaded image is untouched");
        std::error_code ec;
        fs::remove(file, ec);
    }
    {
        std::vector<std::uint8_t> bytes(gm::kGameCfgSize, 0);
        const fs::path file = temp_file("zeros.cfg");
        check(write_file(file, bytes), "the all-zero image is written");
        gm::GameCfg cfg;
        check(!gm::load_game_cfg(file, cfg, &error), "the all-zero image is refused");
        check(contains(error, "header"), "the all-zero text names the header: " + error);
        check(cfg.raw.empty(), "a refused first load leaves an empty image");
        std::error_code ec;
        fs::remove(file, ec);
    }
    {
        const std::vector<std::uint8_t> empty;
        gm::GameCfg cfg;
        check(!gm::parse_game_cfg(empty, cfg, &error), "an empty span is refused");
        check(contains(error, "0 bytes, expected 8192"), "the empty-span text is explicit: " + error);
    }

    // Save refusals.
    {
        gm::GameCfg cfg; // no raw image at all
        check(!gm::save_game_cfg(temp_file("empty_raw.cfg"), cfg, &error),
              "an empty raw image cannot be saved");
        check(contains(error, "raw image is 0 bytes, expected 8192"),
              "the empty-raw text is explicit: " + error);
    }
    {
        gm::GameCfg cfg = base.cfg;
        cfg.header = "GUNMETAL Configuration File. av1.91";
        check(!gm::save_game_cfg(temp_file("bad_header.cfg"), cfg, &error),
              "a mismatched header cannot be saved");
        check(contains(error, "refusing to encode"), "the header text says refusing: " + error);
    }
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec);
        gm::GameCfg cfg = base.cfg;
        check(!gm::save_game_cfg(dir, cfg, &error), "a directory cannot be written as a file");
        check(contains(error, "cannot open"), "the directory text says cannot open: " + error);
        check(!gm::save_game_cfg(dir / "gm_game_cfg_no_such_dir" / "x.cfg", cfg, &error),
              "a missing parent directory is refused");
        check(contains(error, "cannot open"), "the missing-directory text: " + error);
    }
}

void test_install_guard(const Base& base) {
    std::string error;
    const fs::path dir = shipped_dir();
    if (dir.empty()) {
        std::printf("game_cfg: install guard: the installation is absent, only the mirror/scratch "
                    "cases run\n");
    }
    std::vector<fs::path> targets;
    fs::path probe;
    if (!dir.empty()) {
        targets.push_back(dir / "GUNMETAL.CFG");
        std::error_code ec;
        if (fs::is_directory(dir / "SAVEGAME", ec)) {
            targets.push_back(dir / "SAVEGAME" / "GUNMETAL.CFG");
        }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        probe = fs::path(home) / ".cache" / "gunmetal-re" / "game" / "gm_game_cfg_probe.cfg";
        targets.push_back(probe);
    }
    for (const fs::path& target : targets) {
        gm::GameCfg cfg = base.cfg;
        check(!gm::save_game_cfg(target, cfg, &error),
              "saving to " + target.string() + " is refused");
        check(contains(error, "refusing to write into the game installation"),
              "the installation text is explicit: " + error);
    }
    if (!dir.empty()) {
        const fs::path shipped = dir / "GUNMETAL.CFG";
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
        check(read_file(shipped, before), "the shipped GUNMETAL.CFG is readable");
        check(read_file(shipped, after), "the shipped GUNMETAL.CFG is readable again");
        check(before == after && fnv1a64(before) == fnv1a64(base.bytes),
              "the shipped GUNMETAL.CFG is byte-identical after the refused saves");
    }
    if (!probe.empty()) {
        std::error_code ec;
        check(!fs::exists(probe, ec), "the refused mirror probe did not create a file");
    }
    std::printf("game_cfg: install guard targets=%d refused, shipped file untouched\n",
                static_cast<int>(targets.size()));
}

void test_controls() {
    const fs::path dir = shipped_dir();
    if (dir.empty()) {
        std::printf("game_cfg: CONTROLS.CFG: the installation is absent, skipped\n");
        return;
    }
    const fs::path file = dir / "CONTROLS.CFG";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        std::printf("game_cfg: CONTROLS.CFG: not installed, skipped\n");
        return;
    }
    std::vector<std::uint8_t> bytes;
    check(read_file(file, bytes), "CONTROLS.CFG is readable");
    check(bytes.size() == gm::kGameCfgSize, "CONTROLS.CFG is 8192 bytes");
    static constexpr std::string_view kMagic191 = "GUNMETAL Configuration File. av1.91";
    bool magic_ok = bytes.size() >= 35;
    for (std::size_t i = 0; magic_ok && i < kMagic191.size(); ++i) {
        magic_ok = bytes[i] == static_cast<std::uint8_t>(kMagic191[i]);
    }
    check(magic_ok, "CONTROLS.CFG carries the 1.91 magic");

    gm::GameCfg cfg;
    std::string error;
    check(!gm::load_game_cfg(file, cfg, &error), "CONTROLS.CFG is refused by the 1.93 loader");
    check(contains(error, "header") && contains(error, "av1.91"),
          "the refusal names the header and av1.91: " + error);
    check(cfg.raw.empty(), "a refused CONTROLS.CFG leaves no image");
    std::printf("game_cfg: CONTROLS.CFG 8192 B av1.91 refused (opaque body, docs/CONFIG.md §8)\n");
}

void test_determinism(const Base& base) {
    gm::GameCfg first = base.cfg;
    gm::GameCfg second;
    std::string error;
    const bool ok = base.shipped ? gm::load_game_cfg(base.path, second, &error)
                                 : gm::parse_game_cfg(base.bytes, second, &error);
    check(ok, "the second load succeeds");
    check(first.raw == second.raw, "the same raw image");
    check(same_fields(first, second), "the same field values");
    check(gm::game_cfg_version(first) == gm::game_cfg_version(second), "the same version text");

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    check(gm::game_cfg_bytes(first, a, &error), "the first encode succeeds");
    check(gm::game_cfg_bytes(second, b, &error), "the second encode succeeds");
    check(a == b, "the two encodes are identical");

    const fs::path file_a = temp_file("determinism_a.cfg");
    const fs::path file_b = temp_file("determinism_b.cfg");
    check(gm::save_game_cfg(file_a, first, &error), "the first save succeeds");
    check(gm::save_game_cfg(file_b, second, &error), "the second save succeeds");
    std::vector<std::uint8_t> written_a;
    std::vector<std::uint8_t> written_b;
    check(read_file(file_a, written_a) && read_file(file_b, written_b),
          "both determinism copies are readable");
    check(written_a == written_b, "the two files are byte-identical");
    std::error_code ec;
    fs::remove(file_a, ec);
    fs::remove(file_b, ec);
}

} // namespace

int main() {
    const Base base = load_base();
    test_shipped(base);
    test_field_table();
    test_set_field(base);
    test_negatives(base);
    test_install_guard(base);
    test_controls();
    test_determinism(base);

    if (failures == 0) {
        if (g_shipped_ok) {
            std::printf("game_cfg: %d checks, ok (GUNMETAL.CFG 8192 B, 16 fields, 13 settable, "
                        "round-trip 8192/8192 bytes)\n",
                        checks);
        } else {
            std::printf("game_cfg: %d checks, ok (synthetic 8192 B image, shipped GUNMETAL.CFG "
                        "not installed)\n",
                        checks);
        }
    }
    return failures == 0 ? 0 : 1;
}
