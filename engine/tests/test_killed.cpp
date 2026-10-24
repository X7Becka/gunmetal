// Kill-message screen model (`M8.4`, slice 3; docs/KILLED.md).
//
// Pins `ui/killed.h` against the two shipped files (read only: every write goes
// to a scratch directory under /var/tmp) and against the numbers of
// `docs/SUB.md` §7:
//
//   * the files: `FRONTEND/KILLED.skb` 2338 bytes, 92 CRLF lines, 91 non-empty,
//     the first message `%S PUTS %S TO FIRE AND THE SWORD.`, the last
//     `%S KILLS %S.`; `FRONTEND/KILLED.skk` 1967 bytes, 74 lines, 73 non-empty,
//     `%S BITES THE BIG BULLET` .. `%S IS DEAD.`; the two differ in length and
//     in content;
//   * the engine's line filter (`0x485a7d`, `0 < strlen(line) < 0x20` with the
//     `\n` of `fgets` counted) keeps 69 of the 91 `.skb` messages and 50 of the
//     73 `.skk` ones -- the first `.skb` message is 33 bytes and is dropped, so
//     the loaded text starts at `%S MUTILATES %S.`;
//   * `load_killed` prefers `.skb` (`source` names it) and falls back to `.skk`
//     when `.skb` is absent;
//   * `%S` substitution: with `PLAYER` no loaded text contains `%S`/`%s` and
//     every text contains `PLAYER`, while `raw` keeps the placeholder; the
//     counts are 138 of 182 placeholders in `.skb` and 50 of 73 in `.skk` (the
//     rest belong to the dropped lines);
//   * `pick()`: `seed % n` on seeds 0/1/68/69/999, the same text on every call,
//     the last line for 68, the first again for 69, and a one-line screen for
//     every seed;
//   * the model limits: 128 messages (`0x80`), a line of 30 bytes kept and 31
//     dropped, blank and spaces-only lines dropped, a NUL truncating a line,
//     CRLF never reaching a message, LF-only and unterminated files accepted, a
//     15-byte name accepted and 16 refused, an empty name legal only without a
//     placeholder;
//   * negatives: no `FRONTEND`, an empty file, a file whose every line is
//     filtered out, an over-long name, `%S` without a name, a NUL in the name
//     and a 64 KiB+ file are refused with a text, and the refusal leaves the
//     previous screen (lines and source) untouched -- also for non-ASCII and
//     broken-UTF-8 bytes, which load and print as bytes without a crash.
//
// The numbers printed at the end are quoted in docs/KILLED.md §3.

#include <cstddef>
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

#include "ui/killed.h"

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

void check_eq(long long got, long long want, const std::string& what) {
    check(got == want, what + " (got " + std::to_string(got) + ", want " +
                           std::to_string(want) + ")");
}

std::string in_quotes(std::string_view value) { return "\"" + std::string(value) + "\""; }

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_bytes(const fs::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::uint64_t fnv1a64(std::string_view data) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// A scratch directory with a per-process suffix; `/var/tmp` is the project's
/// scratch disk, `GM_KILLED_TMPDIR` overrides it.
fs::path scratch(const std::string& leaf) {
    std::string base = "/var/tmp";
    if (const char* env = std::getenv("GM_KILLED_TMPDIR"); env != nullptr && *env != '\0') {
        base = env;
    }
    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        base = fs::temp_directory_path(ec).string();
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&checks)));
    const fs::path dir = fs::path(base) / ("gm_killed" + std::string(suffix) + "_" + leaf);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// The installation: `GUNMETAL_DIR`, then the reference install, then the
/// mirror under `~/.cache/gunmetal-re/game`.
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
        const fs::path file = root / "FRONTEND" / "KILLED.skb";
        if (fs::is_regular_file(file, ec) && !ec) {
            return root;
        }
    }
    return {};
}

// --- an independent oracle: the counts of the file, without the module -------

/// Split on CRLF only (the shipped files are CRLF, `file(1)` agrees).
std::vector<std::string> crlf_lines(const std::string& data) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t eol = data.find("\r\n", start);
        if (eol == std::string::npos) {
            if (start < data.size()) {
                lines.push_back(data.substr(start));
            }
            break;
        }
        lines.push_back(data.substr(start, eol - start));
        start = eol + 2;
    }
    return lines;
}

std::size_t count_placeholders(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '%' && (text[i + 1] == 'S' || text[i + 1] == 's')) {
            ++count;
            ++i;
        }
    }
    return count;
}

bool blank(std::string_view text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t') {
            return false;
        }
    }
    return true;
}

struct Oracle {
    std::size_t bytes = 0;
    std::size_t lines = 0;
    std::size_t nonempty = 0;
    std::size_t kept = 0;
    std::size_t placeholders = 0;
    std::size_t placeholders_kept = 0;
    std::string first_kept;
    std::string last_kept;
};

Oracle oracle_of(const std::string& data) {
    Oracle out;
    out.bytes = data.size();
    out.placeholders = count_placeholders(data);
    for (const std::string& line : crlf_lines(data)) {
        ++out.lines;
        if (!line.empty()) {
            ++out.nonempty;
        }
        if (line.empty() || blank(line) || line.size() > gm::kKilledMaxLineChars) {
            continue;
        }
        ++out.kept;
        out.placeholders_kept += count_placeholders(line);
        if (out.first_kept.empty()) {
            out.first_kept = line;
        }
        out.last_kept = line;
    }
    return out;
}

// --- the shipped files ------------------------------------------------------

struct Shipped {
    fs::path game;
    fs::path skb;
    fs::path skk;
    std::string skb_bytes;
    std::string skk_bytes;
    Oracle skb_oracle;
    Oracle skk_oracle;
    bool ok = false;
};

Shipped load_shipped() {
    Shipped shipped;
    shipped.game = find_game();
    if (shipped.game.empty()) {
        return shipped;
    }
    shipped.skb = shipped.game / "FRONTEND" / "KILLED.skb";
    shipped.skk = shipped.game / "FRONTEND" / "KILLED.skk";
    shipped.skb_bytes = read_bytes(shipped.skb);
    shipped.skk_bytes = read_bytes(shipped.skk);
    shipped.skb_oracle = oracle_of(shipped.skb_bytes);
    shipped.skk_oracle = oracle_of(shipped.skk_bytes);
    shipped.ok = !shipped.skb_bytes.empty() && !shipped.skk_bytes.empty();
    return shipped;
}

void test_files() {
    const Shipped shipped = load_shipped();
    if (!shipped.ok) {
        std::printf("killed: shipped files not installed, skipping the file checks\n");
        return;
    }
    const Oracle& b = shipped.skb_oracle;
    const Oracle& k = shipped.skk_oracle;
    std::printf("killed: skb bytes=%d lines=%d kept=%d pct=%d/%d fnv=0x%llx\n",
                static_cast<int>(b.bytes), static_cast<int>(b.lines), static_cast<int>(b.kept),
                static_cast<int>(b.placeholders_kept), static_cast<int>(b.placeholders),
                static_cast<unsigned long long>(fnv1a64(shipped.skb_bytes)));
    std::printf("killed: skk bytes=%d lines=%d kept=%d pct=%d/%d fnv=0x%llx\n",
                static_cast<int>(k.bytes), static_cast<int>(k.lines), static_cast<int>(k.kept),
                static_cast<int>(k.placeholders_kept), static_cast<int>(k.placeholders),
                static_cast<unsigned long long>(fnv1a64(shipped.skk_bytes)));

    check_eq(static_cast<long long>(b.bytes), 2338, ".skb bytes");
    check_eq(static_cast<long long>(b.lines), 92, ".skb lines");
    check_eq(static_cast<long long>(b.nonempty), 91, ".skb non-empty lines");
    check_eq(static_cast<long long>(b.kept), 69, ".skb messages kept by 0 < len < 0x20");
    check_eq(static_cast<long long>(b.placeholders), 182, ".skb placeholders in the file");
    check_eq(static_cast<long long>(b.placeholders_kept), 138, ".skb placeholders in kept lines");
    check(b.first_kept == "%S MUTILATES %S.",
          "the first .skb message fits 30 bytes: " + in_quotes(b.first_kept));
    check(b.last_kept == "%S KILLS %S.", "the last .skb message: " + in_quotes(b.last_kept));
    check(shipped.skb_bytes.starts_with("%S PUTS %S TO FIRE AND THE SWORD.\r\n"),
          "the raw .skb starts with a 33-byte message that the engine drops");
    check(shipped.skb_bytes.ends_with("\r\n\r\n"), "the raw .skb ends with a CRLF blank line");

    check_eq(static_cast<long long>(k.bytes), 1967, ".skk bytes");
    check_eq(static_cast<long long>(k.lines), 74, ".skk lines");
    check_eq(static_cast<long long>(k.nonempty), 73, ".skk non-empty lines");
    check_eq(static_cast<long long>(k.kept), 50, ".skk messages kept by 0 < len < 0x20");
    check_eq(static_cast<long long>(k.placeholders), 73, ".skk placeholders in the file");
    check_eq(static_cast<long long>(k.placeholders_kept), 50, ".skk placeholders in kept lines");
    check(k.first_kept == "%S BITES THE BIG BULLET", "the first .skk message");
    check(k.last_kept == "%S IS DEAD.", "the last .skk message: " + in_quotes(k.last_kept));

    check(b.lines != k.lines, "the two files have different line counts");
    check(shipped.skb_bytes != shipped.skk_bytes, "the two files have different bytes");
    check_eq(static_cast<long long>(b.nonempty - b.kept), 22,
             ".skb lines dropped for being longer than 30 bytes");
    check_eq(static_cast<long long>(k.nonempty - k.kept), 23,
             ".skk lines dropped for being longer than 30 bytes");
}

// --- the shipped .skb through the module ------------------------------------

void test_shipped_load() {
    const Shipped shipped = load_shipped();
    if (!shipped.ok) {
        return;
    }
    gm::KilledScreen screen;
    std::string error;
    const bool loaded = gm::load_killed(shipped.game, "PLAYER", screen, &error);
    check(loaded, "the shipped .skb loads: " + error);
    if (!loaded) {
        return;
    }
    g_shipped_ok = true;
    check(screen.source == gm::kKilledPrimaryFile,
          "the preferred file is the source: " + in_quotes(screen.source));
    check_eq(static_cast<long long>(screen.lines.size()), 69, "loaded .skb messages");
    check_eq(static_cast<long long>(gm::substitution_count(screen)), 138,
             "substitutions in the loaded .skb");
    check(screen.lines.front().raw == "%S MUTILATES %S.",
          "the first raw message: " + in_quotes(screen.lines.front().raw));
    check(screen.lines.back().raw == "%S KILLS %S.",
          "the last raw message: " + in_quotes(screen.lines.back().raw));
    check(screen.lines.front().text == "PLAYER MUTILATES PLAYER.",
          "the first substituted text: " + in_quotes(screen.lines.front().text));
    check(screen.lines.back().text == "PLAYER KILLS PLAYER.",
          "the last substituted text: " + in_quotes(screen.lines.back().text));

    std::size_t with_placeholder = 0;
    std::size_t with_name = 0;
    std::size_t carriage_returns = 0;
    std::size_t raw_placeholders = 0;
    for (const gm::KilledLine& line : screen.lines) {
        with_placeholder += contains(line.text, "%S") || contains(line.text, "%s") ? 1 : 0;
        with_name += contains(line.text, "PLAYER") ? 1 : 0;
        carriage_returns += contains(line.raw, "\r") ? 1 : 0;
        raw_placeholders += count_placeholders(line.raw);
    }
    check_eq(static_cast<long long>(with_placeholder), 0, "texts left with a placeholder");
    check_eq(static_cast<long long>(with_name), 69, "texts naming the player");
    check_eq(static_cast<long long>(carriage_returns), 0, "raw lines with a CR");
    check_eq(static_cast<long long>(raw_placeholders), 138, "placeholders counted on raw");

    // A shorter name from the shipped save (`K-9`) changes only the texts.
    gm::KilledScreen short_name;
    check(gm::load_killed(shipped.game, "K-9", short_name, &error),
          "the shipped .skb loads with K-9: " + error);
    check(short_name.lines.size() == screen.lines.size(), "the same message count for K-9");
    check(short_name.lines.front().text == "K-9 MUTILATES K-9.",
          "K-9 in the first text: " + in_quotes(short_name.lines.front().text));
    check(gm::substitution_count(short_name) == gm::substitution_count(screen),
          "the substitution count does not depend on the name");
    check(short_name.lines.front().raw == screen.lines.front().raw, "raw is name independent");
    std::printf("killed: loaded source=%s lines=%d subs=%d first=%s\n", screen.source.c_str(),
                static_cast<int>(screen.lines.size()),
                static_cast<int>(gm::substitution_count(screen)),
                in_quotes(screen.lines.front().text).c_str());
}

// --- the fallback: .skk only ------------------------------------------------

void test_fallback_skk() {
    const Shipped shipped = load_shipped();
    if (!shipped.ok) {
        return;
    }
    const fs::path dir = scratch("skk");
    std::error_code ec;
    fs::create_directories(dir / "FRONTEND", ec);
    check(write_bytes(dir / "FRONTEND" / "KILLED.skk", shipped.skk_bytes),
          "the .skk copy is written");
    check(!fs::exists(dir / "FRONTEND" / "KILLED.skb", ec), "no .skb in the scratch install");

    gm::KilledScreen screen;
    std::string error;
    const bool loaded = gm::load_killed(dir, "PLAYER", screen, &error);
    check(loaded, "a .skk-only install loads: " + error);
    if (!loaded) {
        fs::remove_all(dir, ec);
        return;
    }
    check(screen.source == gm::kKilledFallbackFile,
          "the fallback file is the source: " + in_quotes(screen.source));
    check_eq(static_cast<long long>(screen.lines.size()), 50, "loaded .skk messages");
    check_eq(static_cast<long long>(gm::substitution_count(screen)), 50,
             "substitutions in the loaded .skk");
    check(screen.lines.front().raw == "%S BITES THE BIG BULLET",
          "the first .skk raw message");
    check(screen.lines.front().text == "PLAYER BITES THE BIG BULLET",
          "the first .skk text: " + in_quotes(screen.lines.front().text));
    check(screen.lines.back().raw == "%S IS DEAD.", "the last .skk raw message");
    check(screen.lines.back().text == "PLAYER IS DEAD.", "the last .skk text");

    // The two files give different screens for the same name.
    gm::KilledScreen primary;
    check(gm::load_killed(shipped.game, "PLAYER", primary, &error), "the .skb screen loads");
    check(primary.lines.size() != screen.lines.size(), "different message counts per file");
    check(primary.lines.front().text != screen.lines.front().text,
          "different first message per file");
    std::printf("killed: fallback source=%s lines=%d subs=%d\n", screen.source.c_str(),
                static_cast<int>(screen.lines.size()),
                static_cast<int>(gm::substitution_count(screen)));
    fs::remove_all(dir, ec);
}

// --- pick -------------------------------------------------------------------

void test_pick() {
    const Shipped shipped = load_shipped();
    if (!shipped.ok) {
        return;
    }
    gm::KilledScreen screen;
    std::string error;
    if (!gm::load_killed(shipped.game, "PLAYER", screen, &error)) {
        check(false, "pick: the shipped .skb loads: " + error);
        return;
    }
    const std::size_t n = screen.lines.size();
    const std::size_t seed_999 = 999 % n;
    std::printf("killed: pick n=%d seeds 0->0 1->1 68->68 69->0 999->%d\n",
                static_cast<int>(n), static_cast<int>(seed_999));

    check(gm::pick(screen, 0) == screen.lines[0].text, "seed 0 picks line 0");
    check(gm::pick(screen, 1) == screen.lines[1].text, "seed 1 picks line 1");
    check(gm::pick(screen, 68) == screen.lines[68].text, "seed 68 picks the last line");
    check(gm::pick(screen, 69) == screen.lines[0].text, "seed 69 wraps to line 0");
    check_eq(static_cast<long long>(seed_999), 33, "999 % 69");
    check(gm::pick(screen, 999) == screen.lines[33].text, "seed 999 picks line 33");
    check(gm::pick(screen, 999) == "PLAYER SENDS PLAYER TO THE BONEYARD.",
          "line 33 is the boneyard message: " + in_quotes(gm::pick(screen, 999)));
    check(gm::pick(screen, 0) == gm::pick(screen, 0), "pick is deterministic");
    check(gm::pick(screen, 4294967295u) == screen.lines[4294967295u % n].text,
          "the largest seed wraps by the same rule");

    // A one-line screen ignores the seed.
    const fs::path dir = scratch("one");
    const fs::path file = dir / "FRONTEND";
    std::error_code ec;
    fs::create_directories(file, ec);
    check(write_bytes(file / "KILLED.skb", "%S IS DEAD.\r\n"), "the one-line file is written");
    gm::KilledScreen one;
    check(gm::load_killed(dir, "PLAYER", one, &error), "the one-line screen loads: " + error);
    if (one.lines.size() == 1) {
        check(gm::pick(one, 0) == "PLAYER IS DEAD.", "one line: seed 0");
        check(gm::pick(one, 1) == "PLAYER IS DEAD.", "one line: seed 1");
        check(gm::pick(one, 999) == "PLAYER IS DEAD.", "one line: seed 999");
    } else {
        check(false, "the one-line screen has one line");
    }
    check(gm::pick(gm::KilledScreen{}, 7).empty(), "an empty screen picks nothing");

    // The seeded picks of the shipped screen are stable across loads.
    gm::KilledScreen again;
    if (gm::load_killed(shipped.game, "PLAYER", again, &error)) {
        check(gm::pick(again, 999) == gm::pick(screen, 999), "pick survives a reload");
    } else {
        check(false, "the second load of the shipped .skb: " + error);
    }
    fs::remove_all(dir, ec);
}

// --- the limits of the model ------------------------------------------------

void test_limits() {
    const fs::path dir = scratch("limits");
    const fs::path frontend = dir / "FRONTEND";
    std::error_code ec;
    fs::create_directories(frontend, ec);
    const std::string thirty(30, 'A');
    const std::string thirty_one(31, 'A');

    // 128 messages at most (0x80) and the 30/31-byte boundary.
    std::string body;
    body += thirty_one + "\r\n";
    body += thirty + "\r\n";
    for (int i = 0; i < 200; ++i) {
        body += "LINE " + std::to_string(i) + " %S\r\n";
    }
    body += "\r\n";
    body += "   \t \r\n";
    check(write_bytes(frontend / "KILLED.skb", body), "the limits file is written");
    gm::KilledScreen screen;
    std::string error;
    const bool loaded = gm::load_killed(dir, "PLAYER", screen, &error);
    check(loaded, "the limits file loads: " + error);
    if (loaded) {
        check_eq(static_cast<long long>(screen.lines.size()), 128, "messages capped at 0x80");
        check(screen.lines.front().raw == thirty, "30 bytes are kept, 31 are dropped");
        check(screen.lines[1].raw == "LINE 0 %S", "the first numbered message");
        check(screen.lines.back().raw == "LINE 126 %S", "the last stored message");
        std::printf("killed: limits capped=%d last=%s\n",
                    static_cast<int>(screen.lines.size()),
                    in_quotes(screen.lines.back().raw).c_str());
    }

    // CRLF, LF, a missing terminator and a NUL that truncates a line.
    std::string mixed = "%S ONE\r\n%S TWO\n%S THREE\r\n%S FOUR";
    check(write_bytes(frontend / "KILLED.skb", mixed), "the mixed-EOL file is written");
    if (gm::load_killed(dir, "PLAYER", screen, &error)) {
        check_eq(static_cast<long long>(screen.lines.size()), 4, "mixed EOL lines");
        check(screen.lines[1].raw == "%S TWO", "an LF line loses no byte");
        check(screen.lines[3].raw == "%S FOUR", "the unterminated last line is a message");
        for (const gm::KilledLine& line : screen.lines) {
            check(line.raw.find('\r') == std::string::npos, "no CR in " + in_quotes(line.raw));
        }
    } else {
        check(false, "the mixed-EOL file loads: " + error);
    }

    const std::string with_nul("%S OK\0AND MORE\r\n", 16);
    check(write_bytes(frontend / "KILLED.skb", with_nul), "the NUL file is written");
    if (gm::load_killed(dir, "PLAYER", screen, &error)) {
        check_eq(static_cast<long long>(screen.lines.size()), 1, "one message before the NUL");
        check(screen.lines.front().raw == "%S OK", "the NUL truncates the line: " +
                                                     in_quotes(screen.lines.front().raw));
    } else {
        check(false, "the NUL file loads: " + error);
    }

    // Only `%S`/`%s` are placeholders: another `%` stays.
    check(write_bytes(frontend / "KILLED.skb", "50% OFF, %S AND %s.\r\n"),
          "the percent file is written");
    if (gm::load_killed(dir, "K-9", screen, &error)) {
        check(screen.lines.front().text == "50% OFF, K-9 AND K-9.",
              "a lone % survives, %S and %s do not: " + in_quotes(screen.lines.front().text));
        check_eq(static_cast<long long>(gm::substitution_count(screen)), 2, "two placeholders");
    } else {
        check(false, "the percent file loads: " + error);
    }

    // Non-ASCII and broken UTF-8 bytes are printed as bytes, without a crash.
    std::string high;
    high += "%S \x80\x81\xff\xfe\r\n";             // invalid UTF-8 continuation bytes
    high += "%S \xc3\x28 CAF\xc3\xa9\r\n";        // a broken 2-byte sequence next to a good one
    high += "%S ";
    high += std::string(26, '\xdb');              // 28 bytes with the placeholder: kept
    high += "\r\n";
    check(write_bytes(frontend / "KILLED.skb", high), "the high-byte file is written");
    gm::KilledScreen broken;
    if (gm::load_killed(dir, "\xc3\xa9", broken, &error)) {
        check_eq(static_cast<long long>(broken.lines.size()), 3, "high-byte messages");
        check_eq(static_cast<long long>(broken.lines.front().text.size()), 7,
                 "bytes, not code points: name 2 + space + 4 high bytes");
        check_eq(static_cast<long long>(broken.lines[2].raw.size()), 29, "a 29-byte raw line");
        check(broken.lines[1].text.starts_with("\xc3\xa9 "), "the name is copied as bytes");
        check_eq(static_cast<long long>(gm::substitution_count(broken)), 3,
                 "three high-byte substitutions");
        std::printf("killed: high-bytes lines=%d first_bytes=%d\n",
                    static_cast<int>(broken.lines.size()),
                    static_cast<int>(broken.lines.front().text.size()));
    } else {
        check(false, "the high-byte file loads: " + error);
    }

    // A 15-byte name fits, a 16-byte one does not (the save slot is 16 bytes).
    check(write_bytes(frontend / "KILLED.skb", "%S IS DEAD.\r\n"), "the name file is written");
    gm::KilledScreen name_screen;
    check(gm::load_killed(dir, std::string(15, 'N'), name_screen, &error),
          "a 15-byte name fits: " + error);
    check(!gm::load_killed(dir, std::string(16, 'N'), name_screen, &error),
          "a 16-byte name is refused");
    check(contains(error, "16-byte save slot"), "the name text names the limit: " + error);
    std::printf("killed: name 15=ok 16=\"%s\"\n", error.c_str());

    // An empty name is legal only when the text has no placeholder.
    check(!gm::load_killed(dir, "", name_screen, &error), "an empty name with %S is refused");
    check(contains(error, gm::kKilledPlaceholder) && contains(error, "empty"),
          "the empty-name text: " + error);
    check(write_bytes(frontend / "KILLED.skb", "NO PLACEHOLDER HERE.\r\n"),
          "the placeholder-free file is written");
    gm::KilledScreen plain;
    check(gm::load_killed(dir, "", plain, &error), "an empty name without %S loads: " + error);
    check_eq(static_cast<long long>(plain.lines.size()), 1, "the placeholder-free message");
    check_eq(static_cast<long long>(gm::substitution_count(plain)), 0, "nothing substituted");
    check(plain.lines.front().text == "NO PLACEHOLDER HERE.", "the text is unchanged");

    fs::remove_all(dir, ec);
}

// --- negatives --------------------------------------------------------------

void test_negatives() {
    const Shipped shipped = load_shipped();
    gm::KilledScreen screen;
    std::string error;

    // A successful load first: every refusal below must leave it untouched.
    if (shipped.ok) {
        check(gm::load_killed(shipped.game, "PLAYER", screen, &error),
              "the shipped .skb loads before the negatives: " + error);
    } else {
        const fs::path seed_dir = scratch("seed");
        std::error_code ec;
        fs::create_directories(seed_dir / "FRONTEND", ec);
        write_bytes(seed_dir / "FRONTEND" / "KILLED.skb", "%S SEED.\r\n");
        check(gm::load_killed(seed_dir, "PLAYER", screen, &error), "the seed screen loads");
        fs::remove_all(seed_dir, ec);
    }
    const std::size_t kept_lines = screen.lines.size();
    const std::string kept_source = screen.source;
    check(kept_lines > 0 && !kept_source.empty(), "the seed screen is not empty");

    // No FRONTEND at all.
    const fs::path empty_dir = scratch("empty");
    check(!gm::load_killed(empty_dir, "PLAYER", screen, &error), "a missing FRONTEND is refused");
    check(contains(error, "KILLED.skb") && contains(error, "KILLED.skk"),
          "the missing-file text names both files: " + error);
    check(screen.lines.size() == kept_lines && screen.source == kept_source,
          "the refusal leaves the previous screen untouched");
    std::printf("killed: missing=\"%s\"\n", error.c_str());

    // FRONTEND that holds neither file.
    const fs::path frontend_only = empty_dir / "FRONTEND";
    std::error_code ec;
    fs::create_directories(frontend_only, ec);
    check(!gm::load_killed(empty_dir, "PLAYER", screen, &error),
          "an empty FRONTEND is refused");
    check(contains(error, "neither"), "the text says neither file is there: " + error);

    // An empty file.
    check(write_bytes(frontend_only / "KILLED.skb", ""), "the empty file is written");
    check(!gm::load_killed(empty_dir, "PLAYER", screen, &error), "a 0-byte file is refused");
    check(contains(error, "empty file (0 bytes)"), "the empty text: " + error);
    std::printf("killed: zero=\"%s\"\n", error.c_str());

    // Every line filtered out: blank, blank-ish and over-long.
    check(write_bytes(frontend_only / "KILLED.skb",
                      "\r\n   \r\n" + std::string(31, 'Z') + "\r\n"),
          "the filtered-out file is written");
    check(!gm::load_killed(empty_dir, "PLAYER", screen, &error),
          "a file with nothing to keep is refused");
    check(contains(error, "no message"), "the empty-message text: " + error);
    std::printf("killed: nomessages=\"%s\"\n", error.c_str());

    // A file over the model cap.
    std::string big(gm::kKilledMaxBytes + 1, 'A');
    check(write_bytes(frontend_only / "KILLED.skb", big), "the oversized file is written");
    check(!gm::load_killed(empty_dir, "PLAYER", screen, &error), "a 64 KiB+ file is refused");
    check(contains(error, "larger than"), "the size text: " + error);

    // A NUL in the name.
    check(write_bytes(frontend_only / "KILLED.skb", "%S IS DEAD.\r\n"),
          "the good file is written");
    const std::string nul_name("PLAY\0ER", 8);
    check(!gm::load_killed(empty_dir, nul_name, screen, &error), "a NUL in the name is refused");
    check(contains(error, "NUL"), "the NUL text: " + error);

    // A 16-byte name against a good file.
    check(!gm::load_killed(empty_dir, std::string(16, 'N'), screen, &error),
          "a 16-byte name is refused on a good file");
    check(contains(error, "16-byte save slot"), "the long-name text: " + error);

    // None of the refusals touched the screen loaded at the top.
    check(screen.lines.size() == kept_lines && screen.source == kept_source,
          "every refusal left the screen untouched");
    fs::remove_all(empty_dir, ec);
}

// --- determinism ------------------------------------------------------------

void test_determinism() {
    const Shipped shipped = load_shipped();
    if (!shipped.ok) {
        return;
    }
    gm::KilledScreen first;
    gm::KilledScreen second;
    std::string error;
    check(gm::load_killed(shipped.game, "PLAYER", first, &error), "the first load: " + error);
    check(gm::load_killed(shipped.game, "PLAYER", second, &error), "the second load: " + error);
    check(first.lines.size() == second.lines.size(), "the same message count");
    check(first.source == second.source, "the same source");
    check(gm::substitution_count(first) == gm::substitution_count(second),
          "the same substitution count");
    bool same = first.lines.size() == second.lines.size();
    for (std::size_t i = 0; same && i < first.lines.size(); ++i) {
        same = first.lines[i].raw == second.lines[i].raw &&
               first.lines[i].text == second.lines[i].text;
    }
    check(same, "the same raw and text lines");
    std::printf("killed: deterministic lines=%d subs=%d\n",
                static_cast<int>(second.lines.size()),
                static_cast<int>(gm::substitution_count(second)));
}

} // namespace

int main() {
    test_files();
    test_shipped_load();
    test_fallback_skk();
    test_pick();
    test_limits();
    test_negatives();
    test_determinism();

    if (failures == 0) {
        if (g_shipped_ok) {
            std::printf("killed: %d checks, ok (skb raw=92 kept=69 subs=138, "
                        "skk raw=74 kept=50 subs=50)\n",
                        checks);
        } else {
            std::printf("killed: %d checks, ok (shipped files not installed)\n", checks);
        }
    }
    return failures == 0 ? 0 : 1;
}
