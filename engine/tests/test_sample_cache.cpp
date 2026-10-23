// Runtime sample cache (ROADMAP M7.4, docs/SAMPLE_CACHE.md).
//
// The cache is a model: the 300-slot `SOUND_SAMPLE_TABLE`, a reference count,
// LRU eviction and the `0x100 + decoded bytes` accounting come from the
// original (`0x4c9c90`, `0x4cebd8`/`0x4cf1a8`, `0x4c8770`), while the budget
// *limit*, the handle serial and LRU order are ours.
//
// Names are not hard-coded: the test scans level0.sbt, keeps named slots with
// `count > 0` whose entry really exists in `sounds/SOU.IDX`, and uses the first
// three.  Everything is checked on that real data, then on a copy of the table
// with one count forced to zero (the "absent" case, which no shipped level has
// for a named slot).
#include "audio/sample_cache.h"

#include "game_path.h"
#include "res/sbt.h"
#include "res/sound_catalog.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

constexpr const char* kGameDir = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

struct Pick {
    std::uint32_t slot = 0;
    std::string name;
};

/// Names that are present in the level table and resolve through the *sounds*
/// archive (not the music fallback), in slot order — data, not guesses.
std::vector<Pick> resolvable_names(const gm::SoundSampleTable& table,
                                   const gm::SoundCatalog& catalog) {
    std::vector<Pick> out;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < table.names.size(); ++i) {
        const std::string& name = table.names[i];
        if (name.empty() || table.counts[i] == 0) {
            continue;
        }
        std::string key = name;
        for (char& c : key) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        if (!seen.insert(key).second) {
            continue;
        }
        const auto ref = catalog.find(name);
        if (ref && ref->archive == "sounds" && ref->size > 0) {
            out.push_back(Pick{static_cast<std::uint32_t>(i), name});
        }
    }
    return out;
}

bool same_stats(const gm::audio::SampleCache::Stats& a,
                const gm::audio::SampleCache::Stats& b) {
    return a.entries == b.entries && a.bytes == b.bytes && a.hits == b.hits &&
           a.misses == b.misses && a.evictions == b.evictions &&
           a.peak_bytes == b.peak_bytes;
}

void print_stats(const char* label, const gm::audio::SampleCache& cache) {
    const auto s = cache.stats();
    std::printf("sample_cache: %s entries=%zu bytes=%zu hits=%" PRIu64 " misses=%" PRIu64
                " evictions=%" PRIu64 " peak=%zu limit=%zu\n",
                label, s.entries, s.bytes, s.hits, s.misses, s.evictions, s.peak_bytes,
                cache.bytes_limit());
}

} // namespace

int main() {
    fs::path game = kGameDir;
    std::error_code ec;
    if (!fs::is_regular_file(game / "sounds" / "SOU.IDX", ec)) {
        std::printf("sample_cache: game not installed, skipped\n");
        return 0;
    }

    const auto level_path = gmtest::resolve_game_file(game, "level0.sbt");
    CHECK(level_path.has_value());
    if (!level_path) {
        std::printf("sample_cache: level0.sbt not found\n");
        return 1;
    }
    std::string error;
    const auto level_bytes = read_file(*level_path);
    auto level = gm::parse_sbt(level_bytes, error);
    CHECK(level.has_value());
    CHECK(level->sound_samples.has_value());
    if (!level || !level->sound_samples) {
        std::printf("sample_cache: %s\n", error.c_str());
        return 1;
    }
    const gm::SoundSampleTable& table = *level->sound_samples;

    const auto catalog = gm::SoundCatalog::open(game, error);
    CHECK(catalog.has_value());
    if (!catalog) {
        std::printf("sample_cache: %s\n", error.c_str());
        return 1;
    }

    const std::vector<Pick> picks = resolvable_names(table, *catalog);
    std::printf("sample_cache: level0 table %zu slots, %zu resolvable names in sounds/SOU.IDX\n",
                table.names.size(), picks.size());
    CHECK(picks.size() >= 3);
    if (picks.size() < 3) {
        std::printf("sample_cache: need at least three real names, have %zu\n", picks.size());
        return 1;
    }
    const Pick p0 = picks[0];
    const Pick p1 = picks[1];
    const Pick p2 = picks[2];
    std::printf("sample_cache: picked slot %u %s, slot %u %s, slot %u %s\n", p0.slot,
                p0.name.c_str(), p1.slot, p1.name.c_str(), p2.slot, p2.name.c_str());

    // ---- probe: sizes of the three decoded samples -------------------------
    auto probe = gm::audio::SampleCache::open(game, table, error,
                                              gm::audio::SampleCache::kDefaultBudgetBytes);
    CHECK(probe.has_value());
    if (!probe) {
        std::printf("sample_cache: %s\n", error.c_str());
        return 1;
    }
    std::size_t cost[3] = {0, 0, 0};
    std::size_t frames[3] = {0, 0, 0};
    const Pick* all[3] = {&p0, &p1, &p2};
    for (int i = 0; i < 3; ++i) {
        const auto handle = probe->acquire(all[i]->name, &error);
        CHECK(handle.has_value());
        if (!handle) {
            std::printf("sample_cache: acquire %s: %s\n", all[i]->name.c_str(),
                        error.c_str());
            return 1;
        }
        const auto info = probe->find(all[i]->name);
        CHECK(info.has_value());
        const gm::audio::Sound* sound = probe->sound(*handle);
        CHECK(sound != nullptr);
        if (info && sound) {
            cost[i] = info->bytes;
            frames[i] = static_cast<std::size_t>(sound->frames());
            CHECK_EQ(info->refcount, 1u);
            CHECK_EQ(info->bytes, gm::audio::SampleCache::kEntryOverhead +
                                      sound->samples.size() * sizeof(std::int16_t));
            CHECK(info->sound == sound);
        }
        CHECK(probe->release(*handle, &error));
    }
    std::printf("sample_cache: costs %s=%zu B/%zu fr, %s=%zu B/%zu fr, %s=%zu B/%zu fr\n",
                p0.name.c_str(), cost[0], frames[0], p1.name.c_str(), cost[1], frames[1],
                p2.name.c_str(), cost[2], frames[2]);

    // ---- 1. refcount and "one decode, two acquires" -------------------------
    auto cache = gm::audio::SampleCache::open(game, table, error,
                                              gm::audio::SampleCache::kDefaultBudgetBytes);
    CHECK(cache.has_value());
    if (!cache) {
        std::printf("sample_cache: %s\n", error.c_str());
        return 1;
    }
    const auto h1 = cache->acquire(p0.name, &error);
    CHECK(h1.has_value());
    const auto h2 = cache->acquire(p0.name, &error);
    CHECK(h2.has_value());
    CHECK(h1 == h2);
    if (h1 && h2) {
        auto s = cache->stats();
        CHECK_EQ(s.hits, 1u);
        CHECK_EQ(s.misses, 1u);
        CHECK_EQ(s.entries, 1u);
        CHECK_EQ(s.bytes, cost[0]);
        const auto info = cache->find(p0.name);
        CHECK(info.has_value());
        if (info) {
            CHECK_EQ(info->refcount, 2u);
        }
        CHECK(cache->release(*h1, &error));
        CHECK(cache->release(*h2, &error));
        const auto after = cache->find(p0.name);
        CHECK(after.has_value()); // cached, but evictable
        if (after) {
            CHECK_EQ(after->refcount, 0u);
        }
        const bool third = cache->release(*h1, &error);
        CHECK(!third);
        CHECK(error.find("double release") != std::string::npos);
    }
    print_stats("refcount", *cache);

    // ---- 2. LRU eviction of a zero-count entry -----------------------------
    // The budget fits exactly one of the two entries (their sum cannot fit):
    // p0 (released, refcount 0) must leave before p1 can be inserted.
    auto budget_small = gm::audio::SampleCache::open(
        game, table, error, std::max(cost[0], cost[1]));
    CHECK(budget_small.has_value());
    if (budget_small) {
        const auto a = budget_small->acquire(p0.name, &error);
        CHECK(a.has_value());
        if (a) {
            CHECK(budget_small->release(*a, &error));
            const auto before = budget_small->find(p0.name);
            CHECK(before.has_value());
            if (before) {
                CHECK_EQ(before->refcount, 0u);
            }
        }
        const auto b = budget_small->acquire(p1.name, &error);
        CHECK(b.has_value());
        auto s = budget_small->stats();
        CHECK(s.evictions > 0);
        CHECK(!budget_small->find(p0.name).has_value());
        CHECK(budget_small->find(p1.name).has_value());
        print_stats("evict", *budget_small);
        if (b) {
            CHECK(budget_small->release(*b, &error));
        }
    }

    // ---- 3. a pinned entry is never evicted --------------------------------
    // A+B fits exactly; both are referenced, so the third load must fail with
    // a budget error and the resident bytes must not change.
    auto budget_full = gm::audio::SampleCache::open(game, table, error, cost[0] + cost[1]);
    CHECK(budget_full.has_value());
    if (budget_full) {
        const auto a = budget_full->acquire(p0.name, &error);
        const auto b = budget_full->acquire(p1.name, &error);
        CHECK(a.has_value());
        CHECK(b.has_value());
        const auto before = budget_full->stats();
        const auto c = budget_full->acquire(p2.name, &error);
        CHECK(!c.has_value());
        CHECK(error.rfind("budget:", 0) == 0);
        const auto after = budget_full->stats();
        CHECK_EQ(after.entries, 2u);
        CHECK_EQ(after.bytes, before.bytes);
        CHECK_EQ(after.evictions, before.evictions);
        const auto ia = budget_full->find(p0.name);
        const auto ib = budget_full->find(p1.name);
        CHECK(ia.has_value() && ib.has_value());
        if (ia && ib) {
            CHECK_EQ(ia->refcount, 1u);
            CHECK_EQ(ib->refcount, 1u);
        }
        print_stats("budget", *budget_full);
        if (a) {
            CHECK(budget_full->release(*a, &error));
        }
        if (b) {
            CHECK(budget_full->release(*b, &error));
        }
    }

    // ---- 4. missing names and count == 0 are quiet nullopts ----------------
    auto missing = gm::audio::SampleCache::open(game, table, error,
                                                gm::audio::SampleCache::kDefaultBudgetBytes);
    CHECK(missing.has_value());
    if (missing) {
        const auto before = missing->stats();
        const auto unknown = missing->acquire("nosuchsample", &error);
        CHECK(!unknown.has_value());
        CHECK(error.find("SOUND_SAMPLE_TABLE") != std::string::npos);
        // `find` must not load or count a miss.
        const auto found = missing->find("nosuchsample");
        CHECK(!found.has_value());
        const auto after = missing->stats();
        CHECK_EQ(after.misses, before.misses + 1);
        CHECK_EQ(after.entries, 0u);

        gm::SoundSampleTable zeroed = table;
        zeroed.counts[p0.slot] = 0;
        auto count_zero = gm::audio::SampleCache::open(
            game, zeroed, error, gm::audio::SampleCache::kDefaultBudgetBytes);
        CHECK(count_zero.has_value());
        if (count_zero) {
            const auto absent = count_zero->acquire(p0.name, &error);
            CHECK(!absent.has_value());
            CHECK(error.find("count 0") != std::string::npos);
            CHECK_EQ(count_zero->stats().entries, 0u);
        }
        print_stats("missing", *missing);
    }

    // ---- 5. determinism: the same operations give the same stats ----------
    auto run = [&](gm::audio::SampleCache& target) {
        const auto a = target.acquire(p0.name);
        const auto b = target.acquire(p0.name);
        if (a) {
            target.release(*a);
        }
        if (b) {
            target.release(*b);
        }
        const auto d = target.acquire(p1.name);
        if (d) {
            target.release(*d);
        }
        return target.stats();
    };
    auto first = gm::audio::SampleCache::open(game, table, error,
                                              gm::audio::SampleCache::kDefaultBudgetBytes);
    auto second = gm::audio::SampleCache::open(game, table, error,
                                               gm::audio::SampleCache::kDefaultBudgetBytes);
    CHECK(first.has_value() && second.has_value());
    if (first && second) {
        const auto s1 = run(*first);
        const auto s2 = run(*second);
        CHECK(same_stats(s1, s2));
        print_stats("determinism", *first);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "sample_cache: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sample_cache: ok\n");
    return 0;
}
