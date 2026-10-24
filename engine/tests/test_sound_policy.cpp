// Missing-sample policy (ROADMAP M7.6, slice 2, docs/SOUND_POLICY.md).
//
// docs/SOUND_NAMES.md pins the numbers: every name of a level's
// `SOUND_SAMPLE_TABLE` resolves in `sounds/SOU.IDX` on the shipped data (0
// through the `music` fallback, 0 missing), and a miss in the original is
// silence with a -1/-2 code, never a crash.  This test re-counts that on real
// levels (level0, dm1), then drives the paths no shipped table reaches:
// missing names, a zero budget (-2), a broken index entry (-2) and a missing
// installation (not ready, still no crash).
#include "audio/sample_cache.h"
#include "audio/sound_policy.h"

#include "game_path.h"
#include "res/sbt.h"
#include "res/sound_catalog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

std::optional<gm::SoundSampleTable> load_table(const fs::path& game, const char* name) {
    const auto path = gmtest::resolve_game_file(game, name);
    if (!path) {
        return std::nullopt;
    }
    std::string error;
    const auto level = gm::parse_sbt(read_file(*path), error);
    if (!level || !level->sound_samples) {
        return std::nullopt;
    }
    return *level->sound_samples;
}

struct Sweep {
    int named = 0;
    int empty = 0;
    int sounds = 0;
    int music = 0;
    int missing = 0;
};

/// Walks all 300 slots exactly like the loader `0x40b6c2` does (a slot counts
/// when `count > 0` and the name is non-empty) and counts where the policy
/// resolves each name.
Sweep sweep(gm::audio::SoundPolicy& policy, const gm::SoundSampleTable& table) {
    Sweep out;
    for (std::size_t i = 0; i < table.names.size(); ++i) {
        const std::string& name = table.names[i];
        if (name.empty() || table.counts[i] == 0) {
            ++out.empty;
            continue;
        }
        ++out.named;
        int entry_id = 0;
        switch (policy.lookup(name, &entry_id)) {
        case gm::audio::SoundLookup::sounds:
            ++out.sounds;
            CHECK(entry_id > 0);
            break;
        case gm::audio::SoundLookup::music_fallback:
            ++out.music;
            CHECK(entry_id > 0);
            break;
        case gm::audio::SoundLookup::missing:
            ++out.missing;
            CHECK(entry_id == gm::audio::SoundPolicy::kCodeNotFound ||
                  entry_id == gm::audio::SoundPolicy::kCodeUnplayable);
            break;
        }
    }
    return out;
}

bool same_stats(const gm::audio::SoundPolicyStats& a,
                const gm::audio::SoundPolicyStats& b) {
    return a.requested == b.requested && a.resolved == b.resolved &&
           a.fallback == b.fallback && a.missing == b.missing &&
           a.silent_frames == b.silent_frames;
}

/// A synthetic installation with one index entry whose blob is 4 bytes long:
/// the entry exists (so the name resolves), but its header cannot be read —
/// the `0x40afa6` case that returns -2 without decoding anything.
bool synthetic_broken_entry(const fs::path& game) {
    std::error_code ec;
    fs::remove_all(game, ec);
    fs::create_directories(game / "sounds", ec);
    CHECK(!ec);
    if (ec) {
        return false;
    }
    std::vector<std::uint8_t> index = {'B', 'I', 'G', 'F', 'I', 'L', 'E', 0x1a};
    put_u32(index, 1); // count
    const char name[8] = {'Z', 'Z', 'B', 'R', 'O', 'K', 'E', 'N'};
    index.insert(index.end(), name, name + 8);
    put_u32(index, 0); // offset
    const std::vector<std::uint8_t> dat = {'S', 'O', 'U', '1'};
    const bool ok = write_file(game / "sounds" / "SOU.IDX", index) &&
                    write_file(game / "sounds" / "SOU.DAT", dat);
    CHECK(ok);
    return ok;
}

} // namespace

int main() {
    fs::path game = kGameDir;
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    std::error_code ec;
    if (!fs::is_regular_file(game / "sounds" / "SOU.IDX", ec)) {
        std::printf("sound_policy: game not installed, skipped\n");
        return 0;
    }

    const auto level0 = load_table(game, "level0.sbt");
    const auto dm1 = load_table(game, "dm1.sbt");
    CHECK(level0.has_value());
    CHECK(dm1.has_value());
    if (!level0 || !dm1) {
        std::printf("sound_policy: level0/dm1 SOUND_SAMPLE_TABLE not found\n");
        return 1;
    }

    std::string error;
    const auto catalog = gm::SoundCatalog::open(game, error);
    CHECK(catalog.has_value());
    if (!catalog) {
        std::printf("sound_policy: %s\n", error.c_str());
        return 1;
    }
    std::printf("sound_policy: sounds index %zu entries, music fallback: %s\n",
                catalog->size(), catalog->has_music() ? "yes" : "no");
    // Pinned by tools/re/sound_names.py --report (docs/SOUND_NAMES.md §1).
    CHECK_EQ(catalog->size(), std::size_t{999});

    // ---- 1. every named slot of level0 / dm1 resolves in `sounds` ----------
    // Expected numbers from the oracle on the same files:
    //   python3 - <<...  sound_names.level_names + resolve ...
    //   level0: 137 named slots, 137 sounds / 0 music / 0 missing
    //   dm1:    135 named slots, 135 sounds / 0 music / 0 missing
    // docs/SOUND_NAMES.md §0: "через sounds: 5800; через music: 0; ненайденных: 0".
    struct LevelCase {
        const char* file;
        const gm::SoundSampleTable* table;
        int named;
    };
    const LevelCase cases[2] = {
        {"level0.sbt", &*level0, 137},
        {"dm1.sbt", &*dm1, 135},
    };
    for (const LevelCase& item : cases) {
        gm::audio::SoundPolicy policy;
        policy.set_table(*item.table);
        CHECK(policy.set_root(game, &error));
        CHECK(policy.ready());
        if (!policy.ready()) {
            std::printf("sound_policy: %s: %s\n", item.file, error.c_str());
            continue;
        }
        const Sweep s = sweep(policy, *item.table);
        std::printf("sound_policy: %s slots=%zu named=%d empty=%d sounds=%d music=%d "
                    "missing=%d\n",
                    item.file, item.table->names.size(), s.named, s.empty, s.sounds,
                    s.music, s.missing);
        CHECK_EQ(s.named, item.named);
        CHECK_EQ(s.empty, 300 - item.named);
        CHECK_EQ(s.sounds, item.named);
        CHECK_EQ(s.music, 0);
        CHECK_EQ(s.missing, 0);
        const auto stats = policy.stats();
        CHECK_EQ(stats.requested, s.named);
        CHECK_EQ(stats.resolved, s.named);
        CHECK_EQ(stats.fallback, 0);
        CHECK_EQ(stats.missing, 0);
        CHECK_EQ(stats.silent_frames, 0);
        // A lookup sweep is diagnostics only: nothing was logged or recorded.
        CHECK(policy.missing_names().empty());
        CHECK_EQ(policy.warned_count(), std::size_t{0});
    }

    // ---- 2. a real name: sounds -> cache -> frames, resolved grows ---------
    gm::audio::SoundPolicy good;
    good.set_table(*level0);
    CHECK(good.set_root(game, &error));
    CHECK(good.ready());
    int entry_id = 0;
    const int resolved_before = good.stats().resolved;
    CHECK(good.lookup("updown", &entry_id) == gm::audio::SoundLookup::sounds);
    CHECK(entry_id > 0); // 1-based position in sounds/SOU.IDX
    const auto handle = good.load("updown", &error, &entry_id);
    CHECK(handle.has_value());
    CHECK(entry_id > 0);
    const gm::audio::Sound* pcm = handle ? good.sound(*handle) : nullptr;
    CHECK(pcm != nullptr);
    if (pcm) {
        CHECK(pcm->frames() > 0);
        std::printf("sound_policy: updown entry=%d frames=%u rate=%u resolved=%d\n",
                    entry_id, static_cast<unsigned>(pcm->frames()),
                    static_cast<unsigned>(pcm->sample_rate), good.stats().resolved);
    }
    CHECK_EQ(good.stats().resolved, resolved_before + 2); // lookup + load
    CHECK(good.cache() != nullptr);
    if (handle) {
        CHECK(good.release(*handle, &error));
    }
    CHECK(good.play("UPDOWN"));
    CHECK_EQ(good.stats().silent_frames, 0);
    CHECK_EQ(good.stats().missing, 0);

    // `0x40ae3c`: a name that only exists in `MUSIC/SOU.IDX` (docs/SOUND_NAMES.md
    // §2, DM1A01) resolves as the fallback on the index alone.  It is not a slot
    // of this level's table, so loading it is a quiet -2, not a crash.
    if (catalog->has_music()) {
        int music_id = 0;
        const int fallback_before = good.stats().fallback;
        CHECK(good.lookup("dm1a01", &music_id) ==
              gm::audio::SoundLookup::music_fallback);
        CHECK(music_id > 0);
        CHECK_EQ(good.stats().fallback, fallback_before + 1);
        int code = 0;
        CHECK(!good.load("dm1a01", &error, &code));
        CHECK_EQ(code, gm::audio::SoundPolicy::kCodeUnplayable);
        CHECK(error.find("SOUND_SAMPLE_TABLE") != std::string::npos);
    }

    // ---- 3. missing: lookup says so, load is a nullopt, play is silence ----
    gm::audio::SoundPolicy absent;
    absent.set_table(*level0);
    CHECK(absent.set_root(game, &error));
    CHECK(absent.ready());
    const auto base = absent.stats();
    int code = 0;
    CHECK(absent.lookup("", &code) == gm::audio::SoundLookup::missing);
    CHECK_EQ(code, gm::audio::SoundPolicy::kCodeNotFound);
    CHECK(absent.lookup("nosuchsample", &code) == gm::audio::SoundLookup::missing);
    CHECK_EQ(code, gm::audio::SoundPolicy::kCodeNotFound);
    CHECK_EQ(absent.stats().requested, base.requested + 2);
    CHECK_EQ(absent.stats().missing, base.missing + 2);
    // `lookup` is a query; the diagnostic record is written by load/play.
    CHECK(absent.missing_names().empty());
    CHECK_EQ(absent.warned_count(), std::size_t{0});

    const auto load_handle = absent.load("nosuchsample", &error, &code);
    CHECK(!load_handle.has_value());
    CHECK_EQ(code, gm::audio::SoundPolicy::kCodeNotFound);
    CHECK(error.find("silence") != std::string::npos);
    CHECK_EQ(absent.stats().missing, base.missing + 3);
    CHECK_EQ(absent.missing_names().size(), std::size_t{1});
    CHECK_EQ(absent.missing_names().front(), std::string("NOSUCHSA"));

    // Same 8-byte key twice + the original spelling: silence is success.
    CHECK(absent.play("NOSUCHSAMPLE"));
    CHECK(absent.play("nosuchsample"));
    CHECK_EQ(absent.stats().silent_frames, 2);
    CHECK_EQ(absent.stats().missing, base.missing + 5);
    // ... but one list entry and one `Warn` line for the whole key.
    CHECK_EQ(absent.missing_names().size(), std::size_t{1});
    CHECK_EQ(absent.warned_count(), std::size_t{1});
    std::printf("sound_policy: missing nosuchsample silent=%d list=%zu warned=%zu\n",
                absent.stats().silent_frames, absent.missing_names().size(),
                absent.warned_count());

    // Counters stay consistent: every request has exactly one outcome.
    const auto s = absent.stats();
    CHECK_EQ(s.requested, s.resolved + s.fallback + s.missing);

    // ---- 4. -2: entry resolves, but no room to load it (budget) ------------
    gm::audio::SoundPolicy tight;
    tight.set_table(*level0);
    tight.set_budget(0);
    CHECK(tight.set_root(game, &error));
    CHECK(tight.ready());
    CHECK(tight.lookup("updown", &code) == gm::audio::SoundLookup::sounds);
    const auto tight_handle = tight.load("updown", &error, &code);
    CHECK(!tight_handle.has_value());
    CHECK_EQ(code, gm::audio::SoundPolicy::kCodeUnplayable);
    CHECK(error.rfind("budget:", 0) == 0);
    CHECK(tight.play("updown"));
    CHECK_EQ(tight.stats().silent_frames, 1);
    CHECK_EQ(tight.missing_names().size(), std::size_t{1});
    std::printf("sound_policy: budget=0 updown -> load code=%d, play=silence\n", code);

    // ---- 5. -2: a present entry with a broken (4-byte) container header ----
    const fs::path synthetic = fs::temp_directory_path() / "gm_sound_policy_zzbroken";
    if (synthetic_broken_entry(synthetic)) {
        gm::audio::SoundPolicy broken;
        broken.set_table(gm::SoundSampleTable{}); // no level slots, index only
        CHECK(broken.set_root(synthetic, &error));
        CHECK(broken.ready());
        if (broken.ready()) {
            CHECK(broken.lookup("zzbroken", &code) == gm::audio::SoundLookup::missing);
            CHECK_EQ(code, gm::audio::SoundPolicy::kCodeUnplayable);
            CHECK(!broken.load("zzbroken", &error, &code));
            CHECK_EQ(code, gm::audio::SoundPolicy::kCodeUnplayable);
            CHECK(broken.play("zzbroken"));
            CHECK_EQ(broken.stats().silent_frames, 1);
            std::printf("sound_policy: synthetic broken entry -> code=%d\n", code);
        }
        fs::remove_all(synthetic, ec);
    } else {
        std::printf("sound_policy: synthetic broken entry skipped\n");
        fs::remove_all(synthetic, ec);
    }

    // ---- 6. not ready: missing installation / no table, still no crash ----
    gm::audio::SoundPolicy never;
    CHECK(!never.set_root(fs::temp_directory_path() / "gm_sound_policy_absent", &error));
    CHECK(!never.ready());
    CHECK(never.cache() == nullptr);
    code = 777;
    CHECK(never.lookup("updown", &code) == gm::audio::SoundLookup::missing);
    CHECK_EQ(code, gm::audio::SoundPolicy::kCodeNotFound);
    CHECK(!never.load("updown", &error, &code));
    CHECK_EQ(code, gm::audio::SoundPolicy::kCodeNotFound);
    CHECK(never.play("updown"));
    CHECK_EQ(never.stats().silent_frames, 1);
    CHECK_EQ(never.stats().requested, 3);
    CHECK_EQ(never.stats().missing, 3);
    CHECK_EQ(never.missing_names().size(), std::size_t{1});

    gm::audio::SoundPolicy no_table;
    CHECK(!no_table.set_root(game, &error));
    CHECK(!no_table.ready());
    CHECK(error.find("SOUND_SAMPLE_TABLE") != std::string::npos);
    CHECK(no_table.lookup("updown") == gm::audio::SoundLookup::missing);

    // A re-root after a failure must not keep the old state: level0 -> not-ready.
    gm::audio::SoundPolicy reroot;
    reroot.set_table(*level0);
    CHECK(reroot.set_root(game, &error));
    CHECK(reroot.ready());
    CHECK(!reroot.set_root(fs::temp_directory_path() / "gm_sound_policy_absent", &error));
    CHECK(!reroot.ready());

    // ---- 7. determinism: same sequence, same stats and list ---------------
    auto run = [&](gm::audio::SoundPolicy& policy) {
        int local = 0;
        policy.lookup("updown", &local);
        const auto h = policy.load("updown", &error, &local);
        if (h) {
            policy.release(*h, &error);
        }
        policy.lookup("nosuchsample", &local);
        policy.load("nosuchsample", &error, &local);
        policy.play("nosuchsample");
        policy.play("zzmissing");
        return policy.stats();
    };
    gm::audio::SoundPolicy a;
    gm::audio::SoundPolicy b;
    a.set_table(*level0);
    b.set_table(*level0);
    CHECK(a.set_root(game, &error));
    CHECK(b.set_root(game, &error));
    const auto sa = run(a);
    const auto sb = run(b);
    CHECK(same_stats(sa, sb));
    CHECK(a.missing_names() == b.missing_names());
    CHECK_EQ(a.stats().silent_frames, 2);
    // Unique, and ascending by the 8-byte key (docs/SOUND_POLICY.md).
    const auto names = a.missing_names();
    CHECK_EQ(names.size(), std::size_t{2});
    CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size());
    CHECK(std::is_sorted(names.begin(), names.end()));

    if (g_failures != 0) {
        std::fprintf(stderr, "sound_policy: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sound_policy: ok\n");
    return 0;
}
