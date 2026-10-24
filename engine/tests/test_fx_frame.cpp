// Effect frame resolver (`M4.2.6`, slice 2): `-k` -> `.sbw` record -> frame
// texture -> PICS/sprites archive (engine/src/render/fx_frame.*).
//
// Synthetic cases pin every refusal and the frame clock; the real cases resolve
// the negative indices of level0 and dm1 (names, frame counts, frame index at a
// given tick), check `frames_for_level` de-duplication/order/determinism, and
// sweep all 36 shipped `.sbw` files against the numbers of docs/EFFECTS.md §3:
// 6117 raw `texture < 0`, 133 raw `overlay < 0`, 18 overlay-selected indices,
// 6135 effect polygons, 262 (level, effect) pairs, 1274 frame references over
// 212 distinct textures, 0 unresolved.  Numbers and limits: docs/FX_FRAME.md.

#include "render/fx_frame.h"

#include "res/bigfile.h"
#include "res/sbw.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
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

constexpr const char* kGameDir = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

std::vector<std::string> archive_names(const fs::path& idx) {
    std::vector<std::string> names;
    std::string error;
    const auto archive = gm::BigFile::open(idx, error);
    if (!archive) {
        std::fprintf(stderr, "skip: %s\n", error.c_str());
        return names;
    }
    for (const gm::BigEntry& entry : archive->entries()) {
        names.push_back(entry.name);
    }
    return names;
}

std::string upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

// ---------------------------------------------------------------------------
// Synthetic catalog: two archives, eight records covering every branch.

gm::fx::SoaCatalog synthetic_catalog() {
    gm::fx::SoaCatalog catalog;
    catalog.level = "synthetic";
    catalog.textures = {
        {"one_a", 0},  // id 1
        {"ONE_B", 0},  // id 2 - upper case in the archive
        {"sp_a", 1},   // id 3 - sprites archive
        {"flg", 4},    // id 4 - flaganim, not documented for effect frames
        {"absent", 0}, // id 5 - registry entry without an archive sibling
    };
    const auto add = [&catalog](const char* name, std::uint32_t mode, std::uint32_t period,
                                std::vector<std::uint16_t> ids) {
        gm::SbwEffectRef effect;
        effect.name = name;
        effect.mode = mode;
        effect.period = period;
        effect.ids = std::move(ids);
        catalog.effects.push_back(std::move(effect));
    };
    add("ONE", 1, 40, {1, 2});     // -1 cycle, 2 frames
    add("SHOT", 0, 40, {3});       // -2 one-shot, sprites archive
    add("BROKEN", 1, 40, {});      // -3 no frame ids
    add("NOPERIOD", 1, 0, {1});    // -4 cycle with period 0
    add("BADID", 1, 40, {9});      // -5 id outside the registry
    add("FLAG", 1, 40, {4});       // -6 kind 4
    add("ABSENT", 1, 40, {5});     // -7 name missing from the archive
    add("SNAP", 102, 0, {1});      // -8 special mode: always frame 0
    catalog.negative_indices = {-1, -2, -1, -6, -3, -4, -5, -7, -8, -1};
    return catalog;
}

gm::fx::SbmCatalog synthetic_archives() {
    gm::fx::SbmCatalog archives;
    archives.pics = {"ONE_A", "ONE_B"};
    archives.sprites = {"SP_A"};
    return archives;
}

void check_refused(const gm::fx::SoaCatalog& catalog, const gm::fx::SbmCatalog& archives,
                   int index, const char* needle, const char* what,
                   std::int64_t now_tick = 0) {
    std::string error;
    const auto ref = gm::fx::resolve_fx_frame(index, catalog, archives, &error, now_tick);
    CHECK(!ref.has_value());
    CHECK(!error.empty());
    CHECK(error.find(needle) != std::string::npos);
    if (error.find(needle) == std::string::npos) {
        std::fprintf(stderr, "  %s: index %d -> %s\n", what, index, error.c_str());
    }
}

void synthetic_cases() {
    const auto catalog = synthetic_catalog();
    const auto archives = synthetic_archives();

    // The cycle runs 2 frames over 40 ticks; period boundary wraps to frame 0.
    {
        std::string error = "stale";
        const auto ref = gm::fx::resolve_fx_frame(-1, catalog, archives, &error, 0);
        CHECK(ref.has_value());
        CHECK(error.empty());
        if (ref) {
            CHECK_EQ(ref->series_index, 0);
            CHECK_EQ(ref->series_name, std::string("ONE"));
            CHECK_EQ(ref->frame, 0);
            CHECK_EQ(ref->frame_count, 2);
            CHECK(ref->animated);
        }
        const auto mid = gm::fx::resolve_fx_frame(-1, catalog, archives, &error, 20);
        CHECK(mid && mid->frame == 1);
        const auto wrap = gm::fx::resolve_fx_frame(-1, catalog, archives, &error, 40);
        CHECK(wrap && wrap->frame == 0);
        // Case-insensitive archive lookup: `ONE_B` is stored as `ONE_B`.
        CHECK(gm::fx::resolve_fx_frame(-1, catalog, archives, &error, 20).has_value());
    }

    // One-shot single frame: static, and it is the sprites archive.
    {
        std::string error;
        const auto ref = gm::fx::resolve_fx_frame(-2, catalog, archives, &error, 39);
        CHECK(ref.has_value());
        if (ref) {
            CHECK_EQ(ref->series_index, 1);
            CHECK_EQ(ref->series_name, std::string("SHOT"));
            CHECK_EQ(ref->frame_count, 1);
            CHECK(!ref->animated);
        }
    }

    // Special mode 100/101/102 always shows frame 0 and is not "animated" here:
    // the engine redraws text into that one texture (docs/SOA.md §6), the frame
    // index does not move.
    {
        std::string error;
        const auto ref = gm::fx::resolve_fx_frame(-8, catalog, archives, &error, 12345);
        CHECK(ref.has_value());
        if (ref) {
            CHECK_EQ(ref->frame, 0);
            CHECK_EQ(ref->frame_count, 1);
            CHECK(!ref->animated);
        }
    }

    // Refusals, each with its own reason.
    check_refused(catalog, archives, 0, "not an effect id", "index 0");
    check_refused(catalog, archives, 3, "not an effect id", "positive index");
    check_refused(catalog, archives, -9, "no effect entry", "past the table");
    check_refused(catalog, archives, -3, "no frame ids", "empty record");
    check_refused(catalog, archives, -4, "has no frame", "period 0 cycle");
    check_refused(catalog, archives, -5, "outside the texture registry", "bad frame id");
    check_refused(catalog, archives, -6, "kind 4", "undocumented kind");
    check_refused(catalog, archives, -7, "not in PICS/SBM.IDX", "missing archive entry");

    // Empty catalogs, both alone and together.
    {
        const gm::fx::SoaCatalog no_effects;
        std::string error;
        CHECK(!gm::fx::resolve_fx_frame(-1, no_effects, archives, &error));
        CHECK(error.find("empty effect table") != std::string::npos);

        const gm::fx::SbmCatalog no_archives;
        CHECK(!gm::fx::resolve_fx_frame(-1, catalog, no_archives, &error));
        CHECK(error.find("empty SBM catalog") != std::string::npos);

        CHECK(!gm::fx::resolve_fx_frame(-1, no_effects, no_archives, &error));
        CHECK(!error.empty());

        // A null error pointer must not crash either.
        CHECK(!gm::fx::resolve_fx_frame(7, no_effects, no_archives, nullptr));
    }

    // A record with id 0 and a record whose texture has an empty name.
    {
        auto damaged = catalog;
        damaged.effects[0].ids = {0, 1};
        std::string error;
        CHECK(!gm::fx::resolve_fx_frame(-1, damaged, archives, &error));
        CHECK(error.find("texture id 0") != std::string::npos);

        damaged = catalog;
        damaged.textures[0].name.clear();
        CHECK(!gm::fx::resolve_fx_frame(-1, damaged, archives, &error));
        CHECK(error.find("empty name") != std::string::npos);
    }

    // A negative tick is refused, not read before the frame array (0x41de7d has
    // no clamp, docs/EFFECTS.md §1.2).
    check_refused(catalog, archives, -1, "has no frame", "negative tick", -1);

    // `frames_for_level`: distinct indices, series ascending, -1 first, and a
    // second call gives the same sequence.  -3/-4/-5/-6/-7 are refused and
    // reported separately.
    {
        std::vector<std::string> errors;
        const auto frames = gm::fx::frames_for_level(catalog, archives, 0, &errors);
        CHECK_EQ(frames.size(), std::size_t{3});
        CHECK_EQ(errors.size(), std::size_t{5});
        if (frames.size() == 3) {
            CHECK_EQ(frames[0].series_index, 0);
            CHECK_EQ(frames[1].series_index, 1);
            CHECK_EQ(frames[2].series_index, 7);
        }
        std::set<int> unique;
        for (const gm::fx::FxFrameRef& ref : frames) {
            unique.insert(ref.series_index);
        }
        CHECK_EQ(unique.size(), frames.size());

        std::vector<std::string> again_errors;
        const auto again = gm::fx::frames_for_level(catalog, archives, 0, &again_errors);
        CHECK_EQ(again.size(), frames.size());
        CHECK(again_errors == errors);
        bool same = again.size() == frames.size();
        for (std::size_t i = 0; same && i < frames.size(); ++i) {
            same = again[i].series_index == frames[i].series_index &&
                   again[i].series_name == frames[i].series_name &&
                   again[i].frame == frames[i].frame &&
                   again[i].frame_count == frames[i].frame_count &&
                   again[i].animated == frames[i].animated;
        }
        CHECK(same);

        // The tick moves the frame but not the set of series.
        const auto late = gm::fx::frames_for_level(catalog, archives, 21);
        CHECK_EQ(late.size(), frames.size());
        if (late.size() == frames.size() && late[0].series_index == 0) {
            CHECK_EQ(late[0].frame, 1);
        }
        // Everything refused -> empty result, five reasons, no crash.
        const auto none = gm::fx::frames_for_level(catalog, gm::fx::SbmCatalog{}, 0);
        CHECK(none.empty());
    }

    // §4.2: one pass, no ordering key.
    for (int texture : {-7, -1, 0, 5}) {
        for (int material : {0, 1, 4}) {
            for (int lightmap : {0, 1, 0xffff}) {
                CHECK_EQ(gm::fx::draw_order_rank(
                             gm::fx::FxFrameKey{texture, static_cast<std::uint16_t>(material),
                                                static_cast<std::uint16_t>(lightmap)}),
                         gm::fx::kFxSinglePass);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Real data.

struct Loaded {
    gm::fx::SoaCatalog catalog;
    gm::fx::SbmCatalog archives;
};

std::optional<gm::fx::SoaCatalog> load_level(const fs::path& path, const std::string& name) {
    std::string error;
    const auto bytes = read_file(path);
    const auto world = gm::parse_sbw(bytes, error);
    if (!world) {
        std::fprintf(stderr, "FAIL %s: %s\n", path.string().c_str(), error.c_str());
        ++g_failures;
        return std::nullopt;
    }
    return gm::fx::make_soa_catalog(*world, name);
}

std::optional<gm::fx::SbmCatalog> load_archives(const fs::path& game) {
    gm::fx::SbmCatalog archives;
    archives.pics = archive_names(game / "PICS" / "SBM.IDX");
    archives.sprites = archive_names(game / "sprites" / "SBM.IDX");
    if (archives.pics.empty() || archives.sprites.empty()) {
        std::fprintf(stderr, "FAIL: shipped SBM archives are missing or empty\n");
        ++g_failures;
        return std::nullopt;
    }
    return archives;
}

void real_level_cases(const gm::fx::SbmCatalog& archives, const fs::path& game) {
    const auto level0 = load_level(game / "level0.sbw", "level0");
    const auto dm1 = load_level(game / "dm1.sbw", "dm1");
    if (!level0 || !dm1) {
        return;
    }

    // Raw polygon counts of docs/EFFECTS.md §3.2: level0 68/0, dm1 134/0.
    CHECK_EQ(level0->negative_indices.size(), std::size_t{68});
    CHECK_EQ(dm1->negative_indices.size(), std::size_t{134});

    struct Want {
        int index;
        const char* name;
        int frames;
        bool animated;
    };
    const Want level0_want[] = {
        {-1, "FILM", 1, false},     {-2, "BRIEF", 3, true},  {-3, "COMP2", 2, true},
        {-4, "COMP1", 2, true},     {-5, "DCLOCKLG", 1, false}, {-6, "COMPAN2", 2, true},
        {-7, "TELEP1", 6, true},
    };
    for (const Want& want : level0_want) {
        std::string error;
        const auto ref = gm::fx::resolve_fx_frame(want.index, *level0, archives, &error, 0);
        CHECK(ref.has_value());
        if (!ref) {
            std::fprintf(stderr, "  level0 %d: %s\n", want.index, error.c_str());
            continue;
        }
        CHECK_EQ(ref->series_index, -want.index - 1);
        CHECK_EQ(ref->series_name, std::string(want.name));
        CHECK_EQ(ref->frame, 0);
        CHECK_EQ(ref->frame_count, want.frames);
        CHECK_EQ(ref->animated, want.animated);
    }

    // The frame clock: BRIEF is 3 frames over 65 ticks, so 0/22/44/65 map to
    // 0/1/2/0 (§4.1 formula).  The texture ids behind them are 43/44/45.
    const int brief_ticks[] = {0, 22, 44, 65};
    const int brief_frames[] = {0, 1, 2, 0};
    for (int i = 0; i < 4; ++i) {
        std::string error;
        const auto ref =
            gm::fx::resolve_fx_frame(-2, *level0, archives, &error, brief_ticks[i]);
        CHECK(ref.has_value());
        if (ref) {
            CHECK_EQ(ref->frame, brief_frames[i]);
        }
    }
    // TELEP1: 6 frames over 21 ticks, ping-pong ids 63,64,65,66,65,64.
    for (int tick = 0; tick < 21; ++tick) {
        const int want = tick * 6 / 21;
        std::string error;
        const auto ref = gm::fx::resolve_fx_frame(-7, *level0, archives, &error, tick);
        CHECK(ref.has_value());
        if (ref) {
            CHECK_EQ(ref->frame, want);
        }
    }

    // `frames_for_level`: 7 distinct series for level0 and dm1, unique, stable.
    for (const auto* catalog : {&*level0, &*dm1}) {
        std::vector<std::string> errors;
        const auto frames = gm::fx::frames_for_level(*catalog, archives, 0, &errors);
        CHECK(errors.empty());
        CHECK_EQ(frames.size(), std::size_t{7});
        std::set<int> unique;
        for (const gm::fx::FxFrameRef& ref : frames) {
            unique.insert(ref.series_index);
        }
        CHECK_EQ(unique.size(), frames.size());
        const auto again = gm::fx::frames_for_level(*catalog, archives, 0);
        CHECK_EQ(again.size(), frames.size());
        bool same = again.size() == frames.size();
        for (std::size_t i = 0; same && i < frames.size(); ++i) {
            same = again[i].series_index == frames[i].series_index &&
                   again[i].series_name == frames[i].series_name &&
                   again[i].frame == frames[i].frame;
        }
        CHECK(same);
        std::printf("frames_for_level %-7s: %zu series", catalog->level.c_str(), frames.size());
        for (const gm::fx::FxFrameRef& ref : frames) {
            std::printf(" -%d=%s/%d", ref.series_index + 1, ref.series_name.c_str(),
                        ref.frame_count);
        }
        std::printf("\n");
    }

    // dm1 numbers of docs/EFFECTS.md §3.2 (134 polygons, 7 effects).
    struct Dm1Want {
        int index;
        const char* name;
        int frames;
    };
    const Dm1Want dm1_want[] = {{-1, "WATER1", 6}, {-2, "WATER2", 4}, {-3, "COMP2", 2},
                                {-4, "FILM", 1},   {-5, "COMP1", 2},  {-6, "FLAMES", 8},
                                {-7, "TELEP1", 6}};
    for (const Dm1Want& want : dm1_want) {
        std::string error;
        const auto ref = gm::fx::resolve_fx_frame(want.index, *dm1, archives, &error, 0);
        CHECK(ref.has_value());
        if (ref) {
            CHECK_EQ(ref->series_name, std::string(want.name));
            CHECK_EQ(ref->frame_count, want.frames);
        }
    }

    // A level's own effect is its own: the same `-2` is BRIEF in level0 and
    // WATER2 in dm1 (the loader never searches by name, docs/EFFECTS.md §2.1).
    {
        std::string error;
        const auto in_level0 = gm::fx::resolve_fx_frame(-2, *level0, archives, &error, 0);
        const auto in_dm1 = gm::fx::resolve_fx_frame(-2, *dm1, archives, &error, 0);
        CHECK(in_level0 && in_dm1 && in_level0->series_name != in_dm1->series_name);
    }

    // A frame texture that is not in the archive is refused with a text: drop
    // the name from a copy of the catalog view.
    {
        auto chopped = *level0;
        for (gm::fx::FxTextureRef& texture : chopped.textures) {
            if (upper(texture.name) == "BRIEF1A") {
                texture.name = "no_such_texture";
            }
        }
        std::string error;
        CHECK(!gm::fx::resolve_fx_frame(-2, chopped, archives, &error, 0));
        CHECK(error.find("no_such_texture") != std::string::npos);
        CHECK(error.find("PICS/SBM.IDX") != std::string::npos);
    }
}

void corpus_case(const gm::fx::SbmCatalog& archives, const fs::path& game) {
    std::vector<fs::path> worlds;
    for (const auto& entry : fs::directory_iterator(game)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sbw") {
            worlds.push_back(entry.path());
        }
    }
    std::sort(worlds.begin(), worlds.end());
    CHECK_EQ(worlds.size(), std::size_t{36});

    std::size_t raw_texture_negative = 0;
    std::size_t raw_overlay_negative = 0;
    std::size_t overlay_selected = 0;
    std::size_t effect_polys = 0;
    std::size_t pairs = 0;
    std::size_t frame_refs = 0;
    std::size_t unresolved = 0;
    std::set<std::string> exact_names;
    std::set<std::string> folded_names;
    std::set<std::string> frame_textures;

    for (const fs::path& path : worlds) {
        std::string error;
        const auto bytes = read_file(path);
        const auto world = gm::parse_sbw(bytes, error);
        if (!world) {
            std::fprintf(stderr, "FAIL %s: %s\n", path.string().c_str(), error.c_str());
            ++g_failures;
            continue;
        }
        for (const gm::SbwGeometry& geometry : world->geometry) {
            for (const gm::SbwPoly& poly : geometry.polys) {
                const int texture = static_cast<std::int16_t>(poly.texture);
                const int overlay = static_cast<std::int16_t>(poly.overlay);
                if (texture < 0) {
                    ++raw_texture_negative;
                }
                if (overlay < 0) {
                    ++raw_overlay_negative;
                    if (poly.handle >= 1) {
                        ++overlay_selected;
                    }
                }
            }
        }
        const auto catalog =
            gm::fx::make_soa_catalog(*world, path.stem().string());
        effect_polys += catalog.negative_indices.size();

        std::vector<int> indices = catalog.negative_indices;
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        pairs += indices.size();

        for (const int index : indices) {
            std::string reason;
            const auto ref = gm::fx::resolve_fx_frame(index, catalog, archives, &reason, 0);
            if (!ref) {
                ++unresolved;
                std::fprintf(stderr, "  %s %d: %s\n", catalog.level.c_str(), index,
                             reason.c_str());
                continue;
            }
            frame_refs += static_cast<std::size_t>(ref->frame_count);
            exact_names.insert(ref->series_name);
            folded_names.insert(upper(ref->series_name));
            const auto& effect = catalog.effects[static_cast<std::size_t>(ref->series_index)];
            for (const std::uint16_t id : effect.ids) {
                if (id == 0 || static_cast<std::size_t>(id) > catalog.textures.size()) {
                    ++unresolved;
                    continue;
                }
                const auto& texture = catalog.textures[static_cast<std::size_t>(id) - 1];
                frame_textures.insert(std::to_string(texture.kind) + ":" + upper(texture.name));
            }
        }
    }

    std::printf("corpus: %zu levels, texture<0=%zu overlay<0=%zu overlay-selected=%zu\n",
                worlds.size(), raw_texture_negative, raw_overlay_negative, overlay_selected);
    std::printf("corpus: effect-polygons=%zu pairs=%zu series=%zu/%zu frame-refs=%zu "
                "textures=%zu unresolved=%zu\n",
                effect_polys, pairs, exact_names.size(), folded_names.size(), frame_refs,
                frame_textures.size(), unresolved);

    // docs/EFFECTS.md §3.1-§3.3.
    CHECK_EQ(raw_texture_negative, std::size_t{6117});
    CHECK_EQ(raw_overlay_negative, std::size_t{133});
    CHECK_EQ(overlay_selected, std::size_t{18});
    CHECK_EQ(effect_polys, std::size_t{6135});
    CHECK_EQ(pairs, std::size_t{262});
    CHECK_EQ(frame_refs, std::size_t{1274});
    CHECK_EQ(frame_textures.size(), std::size_t{212});
    CHECK_EQ(unresolved, std::size_t{0});
    // The doc's "48 distinct effects" is case-folded; the raw .sbw spellings
    // are 49 because one level says WATLAVA and another watlava.
    CHECK_EQ(folded_names.size(), std::size_t{48});
    CHECK_EQ(exact_names.size(), std::size_t{49});
}

} // namespace

int main() {
    synthetic_cases();

    const fs::path game = kGameDir;
    if (!fs::is_directory(game)) {
        std::printf("fx_frame: game directory not found, synthetic cases only\n");
    } else if (const auto archives = load_archives(game)) {
        real_level_cases(*archives, game);
        corpus_case(*archives, game);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "fx_frame: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fx_frame: all checks passed\n");
    return 0;
}
