// `.sub` substitution tables (`M2.2`/`M11.5`, slice 2): `res/sub.*`.
//
// Synthetic cases pin both layouts (counted `weight from to` lines and the
// tab-separated `sounds` pairs), the first-row-wins rule of `0x42307a`, the
// `ram_class <= weight` filter of `0x42302c` and every refusal/warning.
// Real cases read the three shipped files (`textures` 119, `sprites` 64,
// `sounds` 161 pairs) and bind `textures.sub` to the 381 texture references
// of `level0.sbw`: 16 rows hit a name, 15 distinct textures change, 8 rows
// have both names in the list, the level keeps 373 unique names instead of
// 378.  The `.sub` files spell names in lowercase, the `.sbw` tables mix
// case, so the engine's 8-byte folded key is what matches (11 exact-case
// hits).  Numbers and the proven/model split: docs/SUB_CPP.md.

#include "res/sub.h"

#include "res/sbw.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

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

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a,   \
                         #b);                                                            \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

fs::path scratch_dir() {
    const fs::path dir = fs::temp_directory_path() / "gm_sub_tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path write_text(const fs::path& dir, const std::string& name,
                    const std::string& body) {
    const fs::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return path;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

/// Mirror first (drvfs is slow), the installation second.
std::optional<fs::path> find_game_dir() {
    std::vector<fs::path> candidates;
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        candidates.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    candidates.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec) &&
            fs::is_regular_file(candidate / "PICS" / "textures.sub", ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::size_t unique_from(const gm::SubTable& table) {
    std::vector<std::string> names;
    for (const gm::SubEntry& entry : table.pairs) {
        names.push_back(entry.from);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names.size();
}

std::size_t unique_to(const gm::SubTable& table) {
    std::vector<std::string> names;
    for (const gm::SubEntry& entry : table.pairs) {
        names.push_back(entry.to);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names.size();
}

// ---------------------------------------------------------------------------
// Synthetic: both layouts, first-row-wins, the RAM filter, refusals.

void synthetic_cases() {
    const fs::path dir = scratch_dir();

    // Counted layout: counter, then `<weight> <from> <to>`; CRLF and a trailing
    // empty line are what the shipped files have.
    {
        const fs::path path = write_text(
            dir, "counted.sub",
            "3\r\n20 alumnm4 alumnm5\r\n20 alumnm6 alumnm5\r\n"
            "16 tile1s tile1\r\n\r\n");
        gm::SubTable table;
        std::string error = "stale";
        CHECK(gm::load_sub(path, gm::SubKind::textures, table, &error));
        CHECK(error.empty());
        CHECK_EQ(table.kind, gm::SubKind::textures);
        CHECK_EQ(table.declared, std::size_t{3});
        CHECK_EQ(table.pairs.size(), std::size_t{3});
        CHECK_EQ(table.source, path.string());
        CHECK_EQ(table.pairs[0].weight, 20);
        CHECK_EQ(table.pairs[0].from, std::string("alumnm4"));
        CHECK_EQ(table.pairs[0].to, std::string("alumnm5"));
        CHECK_EQ(table.pairs[2].weight, 16);

        // First row wins: the engine does not overwrite an existing map.
        CHECK(gm::has(table, "alumnm5"));
        CHECK_EQ(gm::resolve(table, "alumnm5"), std::string("alumnm4"));
        CHECK_EQ(gm::resolve(table, "ALUMNM5"), std::string("alumnm4"));
        CHECK_EQ(gm::resolve(table, "tile1"), std::string("tile1s"));
        CHECK(!gm::has(table, "absent"));
        CHECK_EQ(gm::resolve(table, "absent"), std::string("absent"));

        // 8-byte key of the engine's `char name[9]` slots.
        CHECK_EQ(gm::sub_key("abcdefghij"), std::string("ABCDEFGH"));
        CHECK_EQ(gm::sub_key("MiXeD"), std::string("MIXED"));
        CHECK_EQ(gm::sub_key(""), std::string(""));

        // `ram_class <= weight`; 20 is the shipped texture class.
        CHECK(gm::applies(table.pairs[0], 12));
        CHECK(gm::applies(table.pairs[0], 20));
        CHECK(!gm::applies(table.pairs[0], 21));
        CHECK(!gm::applies(table.pairs[0], 28));
        CHECK(gm::applies(table.pairs[2], 16));
        CHECK(!gm::applies(table.pairs[2], 17));

        // Index view over a name list with a duplicate and a missing name.
        const std::vector<std::string> names = {"ALUMNM5", "TILE1", "ALUMNM5",
                                                "OTHER"};
        gm::bind_indices(table, names);
        CHECK(table.bound);
        CHECK_EQ(table.bound_size, std::size_t{4});
        CHECK_EQ(table.to_index[0], 0);
        CHECK_EQ(table.to_index[1], 0);
        CHECK_EQ(table.to_index[2], 1);
        CHECK_EQ(table.from_index[0], -1); // alumnm4 is not in the list
        CHECK_EQ(table.from_index[2], -1); // tile1s is not in the list
        CHECK(gm::has(table, 0));
        CHECK(gm::has(table, 1));
        CHECK(!gm::has(table, 3));
        CHECK_EQ(gm::resolve(table, 0), -1); // replacement absent from the list
        CHECK_EQ(gm::resolve(table, 3), 3);  // no row: identity
        CHECK_EQ(gm::resolve(table, -1), -1);
        CHECK_EQ(gm::resolve(table, 4), -1);
        CHECK_EQ(gm::resolve(table, 99), -1);
        CHECK(!gm::has(table, -1));

        // An unbound table answers nothing by index.
        gm::SubTable unbound;
        CHECK(!unbound.bound);
        CHECK_EQ(gm::resolve(unbound, 0), -1);
        CHECK(!gm::has(unbound, 0));
    }

    // Tab-separated layout: no counter, no weight.
    {
        const fs::path path = write_text(dir, "pairs.sub",
                                         "alarmk\talarmkbig\r\namboilr2\tamboilr1\r\n");
        gm::SubTable table;
        std::string error;
        CHECK(gm::load_sub(path, gm::SubKind::sounds, table, &error));
        CHECK(error.empty());
        CHECK_EQ(table.declared, std::size_t{0});
        CHECK_EQ(table.pairs.size(), std::size_t{2});
        CHECK_EQ(table.pairs[0].weight, -1);
        CHECK_EQ(table.pairs[0].from, std::string("alarmk"));
        CHECK_EQ(table.pairs[0].to, std::string("alarmkbig"));
        CHECK(gm::applies(table.pairs[0], 0));
        CHECK(gm::applies(table.pairs[0], 4096));
        CHECK_EQ(gm::resolve(table, "amboilr1"), std::string("amboilr2"));
        CHECK_EQ(gm::resolve(table, "alarm"), std::string("alarm")); // key is 8 bytes, not a prefix
    }

    // Counter larger than the record list: the data is kept, a warning is
    // reported, and the shipped loader's "abort the table" is documented.
    {
        const fs::path path = write_text(dir, "short.sub",
                                         "5\r\n20 aaa bbb\r\n20 ccc ddd\r\n");
        gm::SubTable table;
        std::string error;
        CHECK(gm::load_sub(path, gm::SubKind::textures, table, &error));
        CHECK_EQ(table.declared, std::size_t{5});
        CHECK_EQ(table.pairs.size(), std::size_t{2});
        CHECK(contains(error, "warning:"));
        CHECK(contains(error, "declared 5 but 2 records"));
        std::printf("synthetic short: %s\n", error.c_str());
    }

    // Counter smaller than the record list: the extra rows are dropped, like
    // the loader's `fscanf` loop that reads exactly `count` records.
    {
        const fs::path path = write_text(
            dir, "long.sub",
            "1\r\n20 aaa bbb\r\n20 ccc ddd\r\n20 eee fff\r\n");
        gm::SubTable table;
        std::string error;
        CHECK(gm::load_sub(path, gm::SubKind::textures, table, &error));
        CHECK_EQ(table.declared, std::size_t{1});
        CHECK_EQ(table.pairs.size(), std::size_t{1});
        CHECK_EQ(table.pairs[0].from, std::string("aaa"));
        CHECK(contains(error, "warning:"));
        CHECK(contains(error, "extra rows are ignored"));
        std::printf("synthetic long: %s\n", error.c_str());
    }

    // `from == to`: kept, warned (the row draws the same picture).
    {
        const fs::path path = write_text(dir, "self.sub", "1\n20 aaa aaa\n");
        gm::SubTable table;
        std::string error;
        CHECK(gm::load_sub(path, gm::SubKind::textures, table, &error));
        CHECK_EQ(table.pairs.size(), std::size_t{1});
        CHECK(contains(error, "warning:"));
        CHECK(contains(error, "from == to"));
        CHECK_EQ(gm::resolve(table, "aaa"), std::string("aaa"));
        std::printf("synthetic self: %s\n", error.c_str());
    }

    // Hard refusals: every one keeps `out` untouched and returns a text.
    {
        const auto refused = [&dir](const std::string& name, const std::string& body,
                                    gm::SubKind kind, const std::string& needle) {
            const fs::path path = write_text(dir, name, body);
            gm::SubTable table;
            std::string error;
            const bool ok = gm::load_sub(path, kind, table, &error);
            if (ok || error.empty() || !contains(error, needle)) {
                std::fprintf(stderr, "FAIL refusal %s: ok=%d error='%s'\n",
                             name.c_str(), static_cast<int>(ok), error.c_str());
                ++g_failures;
            }
            return table;
        };

        const gm::SubTable empty = refused("empty.sub", "", gm::SubKind::textures,
                                           "empty file");
        CHECK_EQ(empty.declared, std::size_t{0});
        CHECK(empty.pairs.empty());
        refused("blank.sub", " \r\n\t\r\n", gm::SubKind::sounds, "empty file");
        refused("nocount.sub", "abc\r\n20 aaa bbb\r\n", gm::SubKind::textures,
                "count is not a number");
        refused("negcount.sub", "-3\r\n20 aaa bbb\r\n", gm::SubKind::textures,
                "negative count");
        refused("negweight.sub", "1\r\n-20 aaa bbb\r\n", gm::SubKind::textures,
                "negative weight");
        refused("numweight.sub", "1\r\nheavy aaa bbb\r\n", gm::SubKind::textures,
                "weight is not a number");
        refused("twofield.sub", "1\r\n20 aaa\r\n", gm::SubKind::textures,
                "expected <weight> <from> <to>");
        refused("fourfield.sub", "1\r\n20 a b c\r\n", gm::SubKind::textures,
                "expected <weight> <from> <to>");
        refused("spaced.sub", "aaa bbb\r\n", gm::SubKind::sounds,
                "expected <from><TAB><to>");
        refused("longname.sub", "1\r\n20 " + std::string(256, 'x') + " bbb\r\n",
                gm::SubKind::textures, "name longer than 255");
        // The counted and the tab-separated layouts are not interchangeable.
        refused("counted_as_sounds.sub", "1\r\n20 aaa bbb\r\n", gm::SubKind::sounds,
                "expected <from><TAB><to>");
        refused("pairs_as_textures.sub", "aaa\tbbb\r\n", gm::SubKind::textures,
                "count is not a number");

        // Wrong extension and a missing file are refused before any parsing.
        const fs::path text = write_text(dir, "table.txt", "1\r\n20 aaa bbb\r\n");
        gm::SubTable table;
        std::string error;
        CHECK(!gm::load_sub(text, gm::SubKind::textures, table, &error));
        CHECK(contains(error, "not a .sub file"));
        CHECK(!gm::load_sub(dir / "missing.sub", gm::SubKind::textures, table, &error));
        CHECK(contains(error, "cannot open"));
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Shipped files: counts, duplicates, ranges, the RAM class of each table.

void real_tables(const fs::path& game) {
    const auto check_table = [](const fs::path& path, gm::SubKind kind,
                                std::size_t declared, std::size_t pairs,
                                int weight, std::size_t uniq_from,
                                std::size_t uniq_to) {
        gm::SubTable table;
        std::string error;
        const bool ok = gm::load_sub(path, kind, table, &error);
        CHECK(ok);
        CHECK(error.empty());
        CHECK_EQ(table.kind, kind);
        CHECK_EQ(table.declared, declared);
        CHECK_EQ(table.pairs.size(), pairs);
        CHECK_EQ(unique_from(table), uniq_from);
        CHECK_EQ(unique_to(table), uniq_to);
        std::size_t self = 0;
        std::size_t wrong_weight = 0;
        int min_weight = 1 << 30;
        int max_weight = -1;
        std::size_t max_from = 0;
        std::size_t max_to = 0;
        for (const gm::SubEntry& entry : table.pairs) {
            if (entry.from == entry.to) {
                ++self;
            }
            if (entry.weight != weight) {
                ++wrong_weight;
            }
            min_weight = std::min(min_weight, entry.weight);
            max_weight = std::max(max_weight, entry.weight);
            max_from = std::max(max_from, entry.from.size());
            max_to = std::max(max_to, entry.to.size());
        }
        CHECK_EQ(self, std::size_t{0});
        CHECK_EQ(wrong_weight, std::size_t{0});
        if (weight < 0) {
            CHECK_EQ(min_weight, -1);
            CHECK_EQ(max_weight, -1);
        } else {
            CHECK_EQ(min_weight, weight);
            CHECK_EQ(max_weight, weight);
        }
        std::printf("  %-13s declared=%-3zu pairs=%-3zu uniq_from=%-3zu uniq_to=%-3zu "
                    "dup_from=%-2zu dup_to=%-2zu weight=%d maxlen=%zu/%zu\n",
                    path.filename().string().c_str(), table.declared, table.pairs.size(),
                    uniq_from, uniq_to, table.pairs.size() - uniq_from,
                    table.pairs.size() - uniq_to, weight, max_from, max_to);
        return table;
    };

    const gm::SubTable textures =
        check_table(game / "PICS" / "textures.sub", gm::SubKind::textures, 119, 119,
                    20, 103, 116);
    const gm::SubTable sprites =
        check_table(game / "sprites" / "sprites.sub", gm::SubKind::sprites, 64, 64,
                    16, 64, 60);
    const gm::SubTable sounds =
        check_table(game / "sounds" / "sounds.sub", gm::SubKind::sounds, 0, 161,
                    -1, 161, 51);

    // Names of the shipped rows: the longest texture key is exactly 8 bytes,
    // `sounds.sub` has one 9-byte name (truncated to 8 by the engine's slot).
    CHECK_EQ(textures.pairs.front().from, std::string("alumnm4"));
    CHECK_EQ(textures.pairs.front().to, std::string("alumnm5"));
    CHECK_EQ(textures.pairs.back().to, std::string("metal7"));
    CHECK_EQ(sprites.pairs.front().from, std::string("expl0001"));
    CHECK_EQ(sprites.pairs.back().from, std::string("Wtrb0013"));
    CHECK_EQ(sounds.pairs.front().from, std::string("alarmk"));
    CHECK_EQ(sounds.pairs.front().to, std::string("alarmkbig"));
    CHECK_EQ(sounds.pairs.back().to, std::string("amwindh"));
    CHECK(gm::has(sounds, "ALARMKBIG"));           // 8-byte key, folded
    CHECK_EQ(gm::resolve(sounds, "alarmkbig"), std::string("alarmk"));

    // `20` and `16` are RAM classes: at 21/17 nothing applies, which is why the
    // tables are dead on any modern machine (docs/SUB.md §3).
    CHECK_EQ(std::count_if(textures.pairs.begin(), textures.pairs.end(),
                           [](const gm::SubEntry& e) { return gm::applies(e, 20); }),
             std::ptrdiff_t{119});
    CHECK_EQ(std::count_if(textures.pairs.begin(), textures.pairs.end(),
                           [](const gm::SubEntry& e) { return gm::applies(e, 21); }),
             std::ptrdiff_t{0});
    CHECK_EQ(std::count_if(sprites.pairs.begin(), sprites.pairs.end(),
                           [](const gm::SubEntry& e) { return gm::applies(e, 16); }),
             std::ptrdiff_t{64});
    CHECK_EQ(std::count_if(sprites.pairs.begin(), sprites.pairs.end(),
                           [](const gm::SubEntry& e) { return gm::applies(e, 17); }),
             std::ptrdiff_t{0});

    // First-row-wins on the real duplicates: `alumnm5` has two rows and only
    // the first (`alumnm4`) is registered; `Blksmk` has five, first is kept.
    CHECK_EQ(gm::resolve(textures, "alumnm5"), std::string("alumnm4"));
    CHECK_EQ(gm::resolve(textures, "ALUMNM6"), std::string("alumnm4"));
    const std::string blksmk = gm::resolve(sprites, "Blksmk");
    CHECK(gm::has(sprites, "blksmk"));
    std::printf("  Blksmk -> %s (5 rows, first wins)\n", blksmk.c_str());

    // Determinism: a second load gives exactly the same rows.
    gm::SubTable again;
    std::string error;
    CHECK(gm::load_sub(game / "PICS" / "textures.sub", gm::SubKind::textures, again,
                       &error));
    CHECK_EQ(again.pairs.size(), textures.pairs.size());
    bool identical = again.pairs.size() == textures.pairs.size();
    for (std::size_t i = 0; identical && i < again.pairs.size(); ++i) {
        identical = again.pairs[i].weight == textures.pairs[i].weight &&
                    again.pairs[i].from == textures.pairs[i].from &&
                    again.pairs[i].to == textures.pairs[i].to;
    }
    CHECK(identical);
    std::printf("  reload identical=%s\n", identical ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// Application to a real level: `textures.sub` over `level0.sbw`'s 381 refs.

void real_level(const fs::path& game) {
    gm::SubTable table;
    std::string error;
    if (!gm::load_sub(game / "PICS" / "textures.sub", gm::SubKind::textures, table,
                      &error)) {
        std::fprintf(stderr, "FAIL level0: %s\n", error.c_str());
        ++g_failures;
        return;
    }

    std::string parse_error;
    const std::vector<std::uint8_t> bytes = read_file(game / "level0.sbw");
    const auto level = gm::parse_sbw(bytes, parse_error);
    if (!level) {
        std::fprintf(stderr, "FAIL level0.sbw: %s\n", parse_error.c_str());
        ++g_failures;
        return;
    }

    std::vector<std::string> names;
    names.reserve(level->header.textures.size());
    for (const gm::SbwTexRef& ref : level->header.textures) {
        names.push_back(ref.name);
    }
    CHECK_EQ(names.size(), std::size_t{381});

    const std::size_t unique_before = [&names] {
        std::vector<std::string> sorted = names;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        return sorted.size();
    }();
    CHECK_EQ(unique_before, std::size_t{378});

    gm::bind_indices(table, names);
    CHECK(table.bound);
    CHECK_EQ(table.bound_size, names.size());

    std::size_t rows_hit = 0;      // rows whose `to` is a texture of the level
    std::size_t rows_both = 0;     // rows whose `from` is there too
    std::size_t rows_no_repl = 0;  // hits whose `from` is missing (resolve -1)
    std::vector<int> changed;      // distinct replaced indices, first row wins
    for (std::size_t i = 0; i < table.pairs.size(); ++i) {
        if (table.to_index[i] < 0) {
            continue;
        }
        ++rows_hit;
        if (table.from_index[i] >= 0) {
            ++rows_both;
        } else {
            ++rows_no_repl;
        }
        if (std::find(changed.begin(), changed.end(), table.to_index[i]) ==
            changed.end()) {
            changed.push_back(table.to_index[i]);
        }
    }
    CHECK_EQ(rows_hit, std::size_t{16});
    CHECK_EQ(changed.size(), std::size_t{15});
    CHECK_EQ(rows_both, std::size_t{8});
    CHECK_EQ(rows_no_repl, std::size_t{8});
    CHECK_EQ(std::count_if(names.begin(), names.end(),
                           [&table](const std::string& name) {
                               return gm::has(table, name);
                           }),
             std::ptrdiff_t{15});

    // Index view: alumnm5 is slot 28 of level0 and is drawn as slot 0
    // (alumnm4); bulkh3 (slot 10) has a substitution whose picture is not a
    // texture of this level, so the index view cannot name it (-1).
    CHECK_EQ(names[28], std::string("ALUMNM5"));
    CHECK_EQ(names[0], std::string("ALUMNM4"));
    CHECK_EQ(gm::resolve(table, 28), 0);
    CHECK_EQ(gm::resolve(table, 10), -1);
    CHECK(gm::has(table, 10));
    CHECK_EQ(gm::resolve(table, 5), 5);   // no row: identity
    CHECK(!gm::has(table, 5));

    // Before/after: 15 slots change and two names merge, 378 -> 373 unique.
    std::vector<std::string> after;
    after.reserve(names.size());
    for (const std::string& name : names) {
        after.push_back(gm::resolve(table, name));
    }
    std::sort(after.begin(), after.end());
    after.erase(std::unique(after.begin(), after.end()), after.end());
    CHECK_EQ(after.size(), std::size_t{373});

    std::size_t changed_slots = 0;
    for (const std::string& name : names) {
        if (gm::resolve(table, name) != name) {
            ++changed_slots;
        }
    }
    CHECK_EQ(changed_slots, std::size_t{15});

    // The model behind `sub_key`: exact string matching finds 11 names, the
    // engine's folded 8-byte slot finds 15 (`.sub` is lowercase, `.sbw` mixed).
    std::size_t exact = 0;
    for (const std::string& name : names) {
        for (const gm::SubEntry& entry : table.pairs) {
            if (entry.to == name) {
                ++exact;
                break;
            }
        }
    }
    CHECK_EQ(exact, std::size_t{11});

    std::printf("  level0: textures=381 uniq=%zu substituted=%zu rows=%zu both=%zu "
                "uniq_after=%zu exact_case=%zu\n",
                unique_before, changed.size(), rows_hit, rows_both, after.size(), exact);
}

} // namespace

int main() {
    synthetic_cases();
    std::printf("sub: synthetic cases done\n");

    const auto game = find_game_dir();
    if (!game) {
        std::printf("sub: game directory not found, synthetic cases only\n");
    } else {
        std::printf("sub: game=%s\n", game->string().c_str());
        real_tables(*game);
        real_level(*game);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "sub: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sub: all checks passed\n");
    return 0;
}
