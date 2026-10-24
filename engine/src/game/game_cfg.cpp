// `GUNMETAL.CFG` reader/writer (`M12.5`/`M1.5`, slice 1; docs/CONFIG.md,
// docs/GAME_CFG.md).
//
// Only the proven parts of the image are interpreted: the 37-byte magic and
// the sixteen `u32` fields of `docs/CONFIG.md` §3.  Everything else (the two
// key-profile names, the joystick block, the four controller records and the
// uninitialised gaps) stays inside `raw`, so a load -> encode round-trip is
// byte-exact without knowing what those bytes mean.

#include "game/game_cfg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/bytes.h"

namespace gm {
namespace {

constexpr std::string_view kName = "GUNMETAL.CFG";

bool fail(std::string text, std::string* error) {
    if (error != nullptr) {
        *error = std::move(text);
    }
    return false;
}

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

std::string in_quotes(std::string_view text) { return "\"" + std::string(text) + "\""; }

void put32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xFF);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

/// The proven field table, in offset order.  Ranges: see the comments below and
/// `docs/GAME_CFG.md` §1 (every value is either a proven table size or marked
/// MODEL).
const std::vector<GameCfgField>& fields() {
    // 0x190..0x1DC, docs/CONFIG.md §3.  The four read-only entries are the ones
    // the game fills itself: `width`/`height`/`colour_depth` come from the mode
    // table 0x4bfd18 (getter 0x486440), so an editor has no proven legal value
    // for them.
    static const std::vector<GameCfgField> table = {
        {"music_volume", 0x190, 0, 32, 0, true,
         "CONFIG.md §3 0x190 'Music Volume:' 0x4bd508; range 0..32 MODEL"},
        {"sfx_volume", 0x194, 0, 32, 0, true,
         "CONFIG.md §3 0x194 'Effects Volume:' 0x4bd4f8; range 0..32 MODEL"},
        {"swap_left_right", 0x19C, 0, 1, 0, true,
         "CONFIG.md §3 0x19C 'Switch Left Right' 0x4bd4e4, toggle 0/1"},
        {"width", 0x1A0, 0, 0, 0, false,
         "CONFIG.md §3 0x1A0 width, mode table 0x4bfd18 / 0x486440"},
        {"height", 0x1A4, 0, 0, 0, false,
         "CONFIG.md §3 0x1A4 height, mode table 0x4bfd18 / 0x486440"},
        {"colour_depth", 0x1A8, 0, 0, 0, false,
         "CONFIG.md §3 0x1A8 8 = 256 colours, cmp 8 at 0x446975"},
        {"border", 0x1B4, 0, 9, 0, true,
         "CONFIG.md §3 0x1B4 'Border:' 0x4bcf1c, scale 0..9 (0x48c773)"},
        {"gamma", 0x1B8, 0, 3, 0, true,
         "CONFIG.md §3 0x1B8 '%s Gamma Correction' 0x4bcf08, table 0x4bc6d8"},
        {"reflections", 0x1C0, 0, 1, 0, true,
         "CONFIG.md §3 0x1C0 'Reflections' 0x4bcefc, checkbox 0x485fc0"},
        {"light_maps", 0x1C4, 0, 2, 0, true,
         "CONFIG.md §3 0x1C4 off/low/high 0x4bcebc/0x4bcee4/0x4bcecc"},
        {"dynamic_camera", 0x1C8, 0, 2, 0, true,
         "CONFIG.md §3 0x1C8 none/some/full 0x4bcf38/0x4bcf50/0x4bcf68"},
        {"play_flags_p1", 0x1CC, 0, kPlayAllOptions, kPlayAllOptions, true,
         "CONFIG.md §3.1 0x1CC six play bits, mask 0x11D400 (0x44d68c)"},
        {"play_flags_p2", 0x1D0, 0, kPlayAllOptions, kPlayAllOptions, true,
         "CONFIG.md §3.1 0x1D0 the second copy (frontend toggles both)"},
        {"deathmatch_type", 0x1D4, 0, 2, 0, true,
         "CONFIG.md §3 0x1D4 table 0x4bc6e8 (Normal/Scavenger/Classic)"},
        {"starting_cash", 0x1D8, 0, 11, 0, true,
         "CONFIG.md §3 0x1D8 table 0x4bc6f8, 12 values 0x8ca0..0x4c4b40"},
        {"deathmatch_flags", 0x1DC, 0, kDmAllFlags, kDmAllFlags, true,
         "CONFIG.md §3 0x1DC bits 0/2/4/5/6 (0x488646..), mask 0x75"},
    };
    return table;
}

int* field_slot(GameCfg& cfg, std::uint32_t offset) {
    switch (offset) {
    case 0x190: return &cfg.music_volume;
    case 0x194: return &cfg.sfx_volume;
    case 0x19C: return &cfg.swap_left_right;
    case 0x1A0: return &cfg.width;
    case 0x1A4: return &cfg.height;
    case 0x1A8: return &cfg.colour_depth;
    case 0x1B4: return &cfg.border;
    case 0x1B8: return &cfg.gamma;
    case 0x1C0: return &cfg.reflections;
    case 0x1C4: return &cfg.light_maps;
    case 0x1C8: return &cfg.dynamic_camera;
    case 0x1CC: return &cfg.play_flags_p1;
    case 0x1D0: return &cfg.play_flags_p2;
    case 0x1D4: return &cfg.deathmatch_type;
    case 0x1D8: return &cfg.starting_cash;
    case 0x1DC: return &cfg.deathmatch_flags;
    default: return nullptr;
    }
}

const int* field_slot(const GameCfg& cfg, std::uint32_t offset) {
    return field_slot(const_cast<GameCfg&>(cfg), offset);
}

/// The game installations that must never be written: `GUNMETAL_DIR`, the
/// shipped path and the mirror used by `regress.py`.
std::vector<std::filesystem::path> install_roots() {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    std::vector<fs::path> canonical;
    canonical.reserve(roots.size());
    for (const fs::path& root : roots) {
        std::error_code ec;
        fs::path path = fs::weakly_canonical(root, ec);
        canonical.push_back(ec ? root : path);
    }
    return canonical;
}

/// True when `path` is `root` or a descendant of it (component-wise, so
/// `/a/bc` is not under `/a/b`).
bool under(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (p == path.end() || *p != *r) {
            return false;
        }
    }
    return true;
}

} // namespace

const std::vector<GameCfgField>& game_cfg_fields() { return fields(); }

const GameCfgField* find_game_cfg_field(std::string_view name) {
    for (const GameCfgField& field : fields()) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

bool parse_game_cfg(std::span<const std::uint8_t> bytes, GameCfg& cfg, std::string* error) {
    if (bytes.size() != kGameCfgSize) {
        return fail(std::string(kName) + ": " + std::to_string(bytes.size()) +
                        " bytes, expected " + std::to_string(kGameCfgSize),
                    error);
    }
    // The loader compares the whole 0x25-byte magic, terminator included
    // (0x484e9b), so the version is part of the format.
    const std::size_t text = kGameCfgMagic.size();
    for (std::size_t i = 0; i < text; ++i) {
        if (bytes[i] != static_cast<std::uint8_t>(kGameCfgMagic[i])) {
            return fail(std::string(kName) + ": header " +
                            in_quotes(format_ascii(std::span<const std::uint8_t>(bytes.data(),
                                                                             kGameCfgHeaderBytes),
                                                   kGameCfgHeaderBytes)) +
                            " is not " + in_quotes(kGameCfgMagic) + " + 0x1a 0x00",
                        error);
        }
    }
    if (bytes[text] != kGameCfgTerminator) {
        char text_byte[64];
        std::snprintf(text_byte, sizeof(text_byte),
                      "%s: header terminator 0x%02x at 0x%zx, expected 0x1a", std::string(kName).c_str(),
                      static_cast<unsigned>(bytes[text]), text);
        return fail(text_byte, error);
    }
    if (bytes[text + 1] != 0x00) {
        char text_byte[64];
        std::snprintf(text_byte, sizeof(text_byte), "%s: header byte 0x%zx is 0x%02x, expected 0x00",
                      std::string(kName).c_str(), text + 1, static_cast<unsigned>(bytes[text + 1]));
        return fail(text_byte, error);
    }

    // Decode into a scratch object: a refusal leaves the caller's image intact.
    GameCfg scratch;
    scratch.header = std::string(kGameCfgMagic);
    scratch.raw.assign(bytes.begin(), bytes.end());
    for (const GameCfgField& field : fields()) {
        int* slot = field_slot(scratch, field.offset);
        if (slot == nullptr) {
            return fail(std::string(kName) + ": internal: no member for field " +
                            std::string(field.name),
                        error);
        }
        *slot = static_cast<int>(rd32(scratch.raw.data() + field.offset));
    }
    cfg = std::move(scratch);
    clear_error(error);
    return true;
}

bool game_cfg_bytes(const GameCfg& cfg, std::vector<std::uint8_t>& out, std::string* error) {
    if (cfg.raw.size() != kGameCfgSize) {
        return fail(std::string(kName) + ": raw image is " + std::to_string(cfg.raw.size()) +
                        " bytes, expected " + std::to_string(kGameCfgSize),
                    error);
    }
    if (std::string_view(cfg.header) != kGameCfgMagic) {
        return fail(std::string(kName) + ": header " + in_quotes(cfg.header) + " is not " +
                        in_quotes(kGameCfgMagic) + ", refusing to encode",
                    error);
    }
    std::vector<std::uint8_t> bytes = cfg.raw;
    std::memcpy(bytes.data(), kGameCfgMagic.data(), kGameCfgMagic.size());
    bytes[kGameCfgMagic.size()] = kGameCfgTerminator;
    bytes[kGameCfgMagic.size() + 1] = 0x00;
    for (const GameCfgField& field : fields()) {
        const int* slot = field_slot(cfg, field.offset);
        if (slot == nullptr) {
            return fail(std::string(kName) + ": internal: no member for field " +
                            std::string(field.name),
                        error);
        }
        put32(bytes.data() + field.offset, static_cast<std::uint32_t>(*slot));
    }
    out = std::move(bytes);
    clear_error(error);
    return true;
}

bool load_game_cfg(const std::filesystem::path& file, GameCfg& cfg, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        if (ec) {
            return fail(std::string(kName) + ": cannot stat " + file.string() + ": " + ec.message(),
                        error);
        }
        return fail(std::string(kName) + ": no such file: " + file.string(), error);
    }
    if (!fs::is_regular_file(file, ec) || ec) {
        return fail(std::string(kName) + ": not a regular file: " + file.string(), error);
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        return fail(std::string(kName) + ": cannot stat " + file.string() + ": " + ec.message(),
                    error);
    }
    if (size != kGameCfgSize) {
        return fail(std::string(kName) + ": " + file.string() + ": " + std::to_string(size) +
                        " bytes, expected " + std::to_string(kGameCfgSize),
                    error);
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return fail(std::string(kName) + ": cannot open " + file.string(), error);
    }
    std::vector<std::uint8_t> bytes(kGameCfgSize);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(kGameCfgSize));
    if (!in || in.gcount() != static_cast<std::streamsize>(kGameCfgSize)) {
        return fail(std::string(kName) + ": cannot read " + std::to_string(kGameCfgSize) +
                        " bytes from " + file.string(),
                    error);
    }
    return parse_game_cfg(bytes, cfg, error);
}

bool save_game_cfg(const std::filesystem::path& file, const GameCfg& cfg, std::string* error) {
    namespace fs = std::filesystem;
    // The installation is read-only; refuse before opening anything.
    std::error_code ec;
    const fs::path parent = file.has_parent_path() ? file.parent_path() : fs::path(".");
    fs::path canonical_parent = fs::weakly_canonical(parent, ec);
    if (ec) {
        canonical_parent = parent;
    }
    for (const fs::path& root : install_roots()) {
        if (under(canonical_parent, root)) {
            return fail(std::string(kName) + ": refusing to write into the game installation " +
                            root.string() + ": " + file.string(),
                        error);
        }
    }

    std::vector<std::uint8_t> bytes;
    if (!game_cfg_bytes(cfg, bytes, error)) {
        return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(std::string(kName) + ": cannot open " + file.string() + " for writing", error);
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
        return fail(std::string(kName) + ": cannot write " + file.string(), error);
    }
    clear_error(error);
    return true;
}

bool set_field(GameCfg& cfg, const std::string& name, int value, std::string* error) {
    const GameCfgField* field = find_game_cfg_field(name);
    if (field == nullptr) {
        std::string known;
        for (const GameCfgField& entry : fields()) {
            if (!known.empty()) {
                known += ", ";
            }
            known += entry.name;
        }
        return fail(std::string(kName) + ": unknown field " + in_quotes(name) +
                        " (proven fields: " + known + ")",
                    error);
    }
    if (!field->settable) {
        return fail(std::string(kName) + ": field " + std::string(field->name) +
                        " is read-only (" + std::string(field->source) + ")",
                    error);
    }
    if (value < field->min_value || value > field->max_value) {
        return fail(std::string(kName) + ": " + std::string(field->name) + " = " +
                        std::to_string(value) + " is outside " +
                        std::to_string(field->min_value) + ".." +
                        std::to_string(field->max_value) + " (" + std::string(field->source) + ")",
                    error);
    }
    if (field->allowed_mask != 0 &&
        (static_cast<std::uint32_t>(value) & ~field->allowed_mask) != 0) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s: %s has bits outside 0x%x: 0x%x",
                      std::string(kName).c_str(), std::string(field->name).c_str(),
                      field->allowed_mask, static_cast<unsigned>(value & ~field->allowed_mask));
        return fail(text, error);
    }
    int* slot = field_slot(cfg, field->offset);
    if (slot == nullptr) {
        return fail(std::string(kName) + ": internal: no member for field " +
                        std::string(field->name),
                    error);
    }
    *slot = value;
    clear_error(error);
    return true;
}

std::string game_cfg_version(const GameCfg& cfg) {
    const std::string_view header(cfg.header);
    const std::size_t pos = header.rfind("av");
    if (pos == std::string_view::npos) {
        return {};
    }
    return std::string(header.substr(pos + 2));
}

} // namespace gm
