#pragma once

// `GUNMETAL.CFG` — the 8192-byte options image of `gundx.exe` 1.36
// (`M12.5`/`M1.5`, slice 1; docs/CONFIG.md, docs/GAME_CFG.md).
//
// This is the **game** options file, not the CLI configuration of the engine
// (`core/config.h`, `-g/--game`, `GUNMETAL_DIR`).  The module reads the shipped
// image, exposes the proven `u32` fields at their documented offsets and writes
// a byte-exact copy to a path the caller chooses.  It never writes into the
// game installation (that is refused with a text) and it owns no globals.
//
// Proven by code (docs/CONFIG.md §2/§3, addresses are `gundx.exe` VAs):
//
//   * the file is exactly **8192 (0x2000)** bytes: the loader `0x484cf0` does
//     `fread(buf, 0x2000, 1, f)` at `0x484e8a`, so a shorter file is rejected by
//     the game and a longer one loses its tail;
//   * bytes 0x00..0x24 are the **37-byte magic**
//     `"GUNMETAL Configuration File. av1.93" + 0x1A + NUL`; `0x484e9b` compares
//     it byte for byte including the terminator, so the version is part of the
//     format and `av1.93` is the only accepted one;
//   * the fields are little-endian `u32` at 0x190..0x1DC (§3), and the writer
//     `0x485440` copies them back verbatim;
//   * bytes 0x025..0x18F and 0x8AC..0x1FFF are **not initialised** by the
//     writer, so they are kept as opaque `raw` and never interpreted (§2.1);
//   * the two key-profile names (0x1E0..0x227), the joystick/axis block
//     (0x248..0x278) and the four controller records (0x27C..0x8AB) are proven
//     too, but belong to the controls slices; they stay inside `raw`, so a
//     round-trip preserves them byte for byte.
//
// What is deliberately **not** in `GameCfg`:
//
//   * `marker` (0x198, always 1: the loader forces it at `0x484fc2`) and the
//     screen mode (`width`/`height`/`colour_depth` are filled from the mode
//     table `0x4bfd18` at 0x486440) are read-only: an editor has no legal value
//     to put there;
//   * `refresh` (0x1AC, "Hz?" -- hypothesis) and `mode_flags` (0x1B0, only bit
//     0x800 is proven tested) are hypotheses, and `unknown_1bc` (0x1BC) has no
//     consumer at all: per the project rules they are not promoted to fields;
//   * there is **no proven "shadows" option** in `docs/CONFIG.md`; the closest
//     proven one is `reflections` (0x1C0, `'Reflections'` 0x4bcefc).
//
// Model, marked `MODEL` where it applies:
//
//   * the accepted range of the two volumes is `0..32`.  The game only proves
//     that music is `'Music Volume:'` (0x4bd508) and effects `'Effects
//     Volume:'` (0x4bd4f8), drawn as `v * 2.7` in 0x488a4f/0x488a9d; whether
//     0..32 is the UI scale is not proven.  Bounds of the other fields are the
//     proven table sizes: gamma 4 entries (0x4bc6d8), border 0..9 (cmp 9 at
//     0x48c773), light maps 3 labels (0x4bcebc/0x4bcee4/0x4bcecc), dynamic
//     camera 3 labels (0x4bcf38/0x4bcf50/0x4bcf68), deathmatch type 3 entries
//     (0x4bc6e8), starting cash 12 values (0x4bc6f8), reflections/left-right
//     toggle 0/1, and the six play-option bits / five DM bits of §3.1 that
//     `0x44d68c` masks with `0x11D400`.
//   * `set_field()` only edits `GameCfg`; it does not touch `raw`, does not
//     write a file and changes nothing in the original game.  `game_cfg_bytes()`
//     applies the field values to a copy of `raw` at their proven offsets, so a
//     loaded-then-unchanged image re-encodes to the same 8192 bytes.
//   * `save_game_cfg()` refuses a target under the game installation
//     (`GUNMETAL_DIR`, `/mnt/d/games/Gunmetal 3dfx`, or the
//     `~/.cache/gunmetal-re/game` mirror), because the original must stay
//     read-only.  Every other path is written as given.
//
// Not wired into `main.cpp`, `GameRuntime` or any screen: nothing here applies
// the values to the engine, and `CONTROLS.CFG` (magic `av1.91`) is refused as a
// different, still opaque format (docs/CONFIG.md §8, docs/GAME_CFG.md).
//
// Invariants (checked by `engine/tests/test_game_cfg.cpp`)
// -------------------------------------------------------
//   * every `GameCfgField` has an offset in 0x190..0x1DC, a member of
//     `GameCfg` and the same value as `raw[offset..offset+4]` after a load;
//   * `game_cfg_bytes()` of a loaded, unmodified image is byte-identical to the
//     file it came from; after `set_field()` the only bytes that differ are the
//     four bytes of that field;
//   * a refused `parse`/`load`/`save`/`set_field` leaves its target untouched
//     and sets a non-empty text that starts with `GUNMETAL.CFG:`.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// 0x2000: the only size the loader accepts (`fread` at 0x484e8a).
inline constexpr std::size_t kGameCfgSize = 0x2000;

/// 0x25: magic + `0x1A` + NUL, compared whole at 0x484e9b.
inline constexpr std::size_t kGameCfgHeaderBytes = 0x25;

/// Byte 0x23 of the image, right after the version text.
inline constexpr std::uint8_t kGameCfgTerminator = 0x1A;

/// The 35 printable bytes of the magic; 0x23 = 0x1A and 0x24 = 0x00 follow.
inline constexpr std::string_view kGameCfgMagic = "GUNMETAL Configuration File. av1.93";

/// The six play-option bits (`docs/CONFIG.md` §3.1, mask of 0x44d68c).
inline constexpr int kPlaySkipEmptyWeapons = 0x00400;
inline constexpr int kPlayDontAutoTarget = 0x01000;
inline constexpr int kPlaySkipDeadlyWeapons = 0x04000;
inline constexpr int kPlaySwitchToPickedUpAmmo = 0x08000;
inline constexpr int kPlayAutoSwitchDepleted = 0x10000;
inline constexpr int kPlayMouseCheating = 0x100000;
inline constexpr int kPlayAllOptions = 0x11D400;

/// The five deathmatch flag bits (`docs/CONFIG.md` §3, 0x1DC).
inline constexpr int kDmTeamplay = 1 << 0;
inline constexpr int kDmNoEnemies = 1 << 2;
inline constexpr int kDmCantBuyAmmo = 1 << 4;
inline constexpr int kDmHandicap = 1 << 5;
inline constexpr int kDmMouseCheating = 1 << 6;
inline constexpr int kDmAllFlags = 0x75;

/// One proven `u32` field: name, offset, accepted range and provenance.
struct GameCfgField {
    std::string_view name;
    std::uint32_t offset;
    int min_value;              ///< inclusive
    int max_value;              ///< inclusive
    std::uint32_t allowed_mask; ///< 0 = any 32-bit value; else only these bits
    bool settable;              ///< false: the game owns the value
    std::string_view source;    ///< docs/CONFIG.md section + VA of the evidence
};

/// The field table in offset order (16 entries; 13 settable).
const std::vector<GameCfgField>& game_cfg_fields();

/// The field with that exact name, or `nullptr`.
const GameCfgField* find_game_cfg_field(std::string_view name);

/// The decoded image plus the raw bytes it came from.
struct GameCfg {
    /// The 35 magic bytes 0x00..0x22, without the 0x1A/NUL terminator.
    std::string header;
    /// All 8192 bytes verbatim; holds the names, the controller records and
    /// the uninitialised gaps, so encoding is byte-exact.
    std::vector<std::uint8_t> raw;

    // Proven fields, docs/CONFIG.md §3.  Offsets are the image offsets.
    int music_volume = 0;     // 0x190 'Music Volume:' (0x4bd508)
    int sfx_volume = 0;       // 0x194 'Effects Volume:' (0x4bd4f8)
    int swap_left_right = 0;  // 0x19C 'Switch Left Right' (0x4bd4e4)
    int width = 0;            // 0x1A0 screen width (0x486440, table 0x4bfd18)
    int height = 0;           // 0x1A4 screen height (same mode table)
    int colour_depth = 0;     // 0x1A8 8 = 256 colours (cmp 8 at 0x446975)
    int border = 0;           // 0x1B4 'Border:' (0x4bcf1c), 0..9 (0x48c773)
    int gamma = 0;            // 0x1B8 0..3 (table 0x4bc6d8)
    int reflections = 0;      // 0x1C0 'Reflections' (0x4bcefc)
    int light_maps = 0;       // 0x1C4 0..2 (0x4bcebc/0x4bcee4/0x4bcecc)
    int dynamic_camera = 0;   // 0x1C8 0..2 (0x4bcf38/0x4bcf50/0x4bcf68)
    int play_flags_p1 = 0;    // 0x1CC six play-option bits (mask 0x11D400)
    int play_flags_p2 = 0;    // 0x1D0 the second copy (frontend toggles both)
    int deathmatch_type = 0;  // 0x1D4 0..2 (table 0x4bc6e8)
    int starting_cash = 0;    // 0x1D8 0..11 (table 0x4bc6f8)
    int deathmatch_flags = 0; // 0x1DC five DM bits (0x488646..)
};

/// Parse an 8192-byte image.  Refuses the wrong size, a header that is not
/// `kGameCfgMagic`, a terminator other than `0x1A` and a missing NUL; `cfg` is
/// left untouched on failure.
bool parse_game_cfg(std::span<const std::uint8_t> bytes, GameCfg& cfg, std::string* error);

/// `raw` with the 16 field values and the magic written at their offsets.
/// A loaded, unmodified image gives back the same bytes.
bool game_cfg_bytes(const GameCfg& cfg, std::vector<std::uint8_t>& out, std::string* error);

/// Read and parse `file`.  Errors: cannot stat/open/read, size != 8192, header.
bool load_game_cfg(const std::filesystem::path& file, GameCfg& cfg, std::string* error);

/// Write `cfg` to `file`.  The image is encoded first; a path under the game
/// installation (or its mirror) is refused before anything is opened.
bool save_game_cfg(const std::filesystem::path& file, const GameCfg& cfg, std::string* error);

/// Set one proven field by name.  Unknown names, read-only fields (`width`,
/// `height`, `colour_depth`), a value outside the accepted range and a bit
/// outside `allowed_mask` are refused with a text; the field keeps its value.
bool set_field(GameCfg& cfg, const std::string& name, int value, std::string* error);

/// `"1.93"` from the header (the last two characters after `av`), or `""`.
std::string game_cfg_version(const GameCfg& cfg);

} // namespace gm
