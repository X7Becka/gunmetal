#pragma once

// Kill-message screen (`M8.4`, slice 3): `FRONTEND/KILLED.skk` / `KILLED.skb`.
//
// The front end owns two arrays of one-line kill messages and fills them from
// two shipped text files of the same shape: one message per line, `%S` where
// the name of the dead player goes.  This module is the data side of that
// screen -- the kept messages with their substitution done -- and owns no
// pixels, no font and no screen state.
//
// Proven behaviour (`docs/SUB.md` §7, VAs in `gundx.exe`)
// ------------------------------------------------------
//   * one function turns the files into messages: `0x485a00` opens
//     `FRONTEND/KILLED.skk` (into the array of counter `0x68ddb8`) and
//     `0x485b2f` opens `FRONTEND/KILLED.skb` (counter `0x68ddb4`); both use
//     `fopen(..., "r")` (`0x4a6888`, text mode) and `fgets(buf, 0x400, f)`
//     (`0x40e050`);
//   * a line is kept only when `0 < strlen(line) < 0x20` (`0x485a7d`).  Text
//     mode has already folded CRLF to LF and `fgets` keeps the `\n` in the
//     buffer, so the *visible* text of a kept message is 1..30 bytes; the
//     shipped data confirms the threshold: 50 of 73 `.skk` lines and 69 of 91
//     `.skb` lines survive, 23 and 22 are too long;
//   * at most `0x80` messages are stored (`0x68ddb8` / `0x68ddb4` are the two
//     counters);
//   * any `%<char>` is normalised to `%s` before the message is formatted
//     (`0x485ad6`..`0x485ae1`), i.e. `%S` of the file is a placeholder too.
//     Both spellings occur in the shipped data: `.skk` has 44 `%S` + 29 `%s`,
//     `.skb` 109 `%S` + 73 `%s`.
//
// Model, not a port -- every such place is marked `MODEL` in the doc
// -------------------------------------------------------------------
//   * **which file is read** is ours.  The original fills *both* arrays from
//     the two files (their screen binding is not proven), so this loader
//     prefers `KILLED.skb` (91 shipped lines, the larger and the two-name
//     template set) and falls back to `KILLED.skk` when `.skb` is missing;
//     `KilledScreen::source` names the file that was actually read.
//   * **one name, not two.**  `.skb` templates carry two placeholders
//     (`%S KILLS %S.`) -- killer and victim in the deathmatch screen -- while
//     this API takes a single `player_name`, so both become the same name.
//   * the placeholder count is measured on `raw`, so `substitution_count()` is
//     `%S` *and* `%s`, not only the uppercase spelling.
//   * a line of nothing but spaces/tabs is treated as empty and skipped: the
//     engine would keep the spaces as a message, but it would print nothing.
//   * a file larger than `kKilledMaxBytes` (64 KiB) is refused instead of being
//     slurped; the shipped files are 1967 and 2338 bytes.
//   * the player name is limited to 15 bytes, the width of the name slot of a
//     save (`0xfc`..`0x10c`, `docs/SAVE_FIELDS.md` §2.1, "K-9"); a NUL byte
//     inside a name is refused.
//   * an empty name is only legal when the loaded text has no placeholder at
//     all; otherwise `%S` would be printed as is, so the loader refuses it.
//   * `pick()` is ours: the original picks a message with its own RNG, which
//     is not documented here, so the index is `seed % lines.size()`.
//
// Errors are text (`std::string* error`): no file at all, an empty file, a file
// whose every line is filtered out, a name longer than 15 bytes and `%S`
// without a name.  A refusal leaves the previous `KilledScreen` untouched.
//
// Not wired into the frontend, `GameRuntime` or the CLI: nothing draws the
// `KILLED` screen, and there is no `--killed` option.
// See `docs/KILLED.md`.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gm {

/// `0x485a7d`: `strlen(line) < 0x20` counts the `\n` that `fgets` keeps, so a
/// kept message has 1..30 visible bytes.
inline constexpr std::size_t kKilledMaxLineChars = 30;

/// `0x68ddb8` / `0x68ddb4`: at most 128 messages per array.
inline constexpr std::size_t kKilledMaxMessages = 0x80;

/// The player-name slot of a save is 16 bytes (`0xfc`..`0x10c`), terminator
/// included.
inline constexpr std::size_t kKilledMaxNameBytes = 15;

/// Model cap on the file size: the shipped files are 1967 and 2338 bytes.
inline constexpr std::size_t kKilledMaxBytes = 64 * 1024;

/// The substitution token of the file: `%S` (also accepted as `%s`, the
/// normalised spelling the engine stores).
inline constexpr const char* kKilledPlaceholder = "%S";

/// Preferred file: 91 shipped lines, two placeholders per template.
inline constexpr const char* kKilledPrimaryFile = "FRONTEND/KILLED.skb";
/// Fallback: 73 shipped lines, one placeholder per template.
inline constexpr const char* kKilledFallbackFile = "FRONTEND/KILLED.skk";

/// One kept message: the file's line (`raw`, placeholder untouched) and the
/// same line with every placeholder replaced by the player name (`text`).
struct KilledLine {
    std::string raw;
    std::string text;
};

/// The messages of one file, in file order.
struct KilledScreen {
    std::vector<KilledLine> lines;
    /// Path that was read, relative to `game` (`FRONTEND/KILLED.skb` or
    /// `FRONTEND/KILLED.skk`); empty when nothing has been loaded.
    std::string source;
};

/// Read `FRONTEND/KILLED.skb` (or `.skk`) of the installation *game*, substitute
/// `player_name` for every placeholder and keep the lines the engine keeps.
///
/// Returns `false` and fills `error` (when not null) on the refusals listed in
/// the header comment; on success `screen.source` names the file read.
bool load_killed(const std::filesystem::path& game, const std::string& player_name,
                 KilledScreen& screen, std::string* error);

/// The message number `seed % lines.size()`, i.e. a deterministic choice; an
/// empty screen gives an empty string.
std::string pick(const KilledScreen& screen, std::uint32_t seed);

/// How many placeholders (`%S` / `%s`) the loaded lines carry, i.e. how many
/// bytes `player_name` replaced.
std::size_t substitution_count(const KilledScreen& screen);

} // namespace gm
