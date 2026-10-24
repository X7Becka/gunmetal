// Thing animation binding, `M6.3.1` slice 2: `world/thing_anim.h`.
//
// Synthetic part: the state -> clip map, the `_idle`/`_walk`/`_atk`/`_dead`
// probe order (variant first, base clip second), the ASCII case-insensitive
// lookup, every refusal (`thing_anim:` prefix) and the 91 Hz hold/loop split
// on hand-written clips in a temporary `Sobanim/` directory.
//
// Real part, on the shipped installation:
//  * the whole `Sobanim/` corpus (40 `.soo`): how many bind, how many have a
//    state-specific variant (0), how many are mode 0 (26) / mode 1 (14), and
//    that every frame index stays inside the frames the engine would load;
//  * a real type of `level13.sbt` whose model is a clip (`CULTIST` ->
//    `Cultist.soo`, mode 1, 4 frames, 400 ticks) bound at idle/walk/attack/
//    dead, including the death hold at the last frame;
//  * the same clip in mode 0 (`kndeath`, 4 x 15 = 60 ticks) and mode 1
//    (`kntbox`, 10 x 20 = 200 ticks) at ticks 0 / duration / duration + 1;
//  * a static `.sob` model of `dm1.sbt` as the "no clip" case;
//  * determinism of two identical passes.
//
// Numbers and the proven/model split: docs/THING_ANIM_BIND.md.

#include "world/thing_anim.h"

#include "res/anim.h"
#include "res/sbt.h"
#include "res/vm_exec.h"
#include "world/ai.h"
#include "world/things.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

using gm::AiState;
using gm::Thing;
using gm::thinganim::ThingAnimBinding;
using gm::thinganim::ThingAnimConfig;
using gm::thinganim::ThingAnimStats;
using gm::thinganim::ThingClip;

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// A thing whose only meaningful field is the model name.
Thing model_thing(std::string model) {
    Thing thing;
    thing.id = 1;
    thing.model = std::move(model);
    return thing;
}

bool same_binding(const std::optional<ThingAnimBinding>& a,
                  const std::optional<ThingAnimBinding>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a) {
        return true;
    }
    return a->clip == b->clip && a->clip_name == b->clip_name && a->frame == b->frame &&
           a->tick == b->tick && a->looping == b->looping;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

/// The installation with `Sobanim/`: `$GUNMETAL_DIR`, the read-only mirror,
/// then the reference install (the same order as `resolve_anim_root`).
std::optional<fs::path> find_game_dir() {
    const fs::path root = gm::thinganim::resolve_anim_root();
    if (root.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::is_directory(root / "Sobanim", ec)) {
        return std::nullopt;
    }
    return root;
}

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// ---------------------------------------------------------------------------
// Synthetic: state map, probe order, refusals, hold/loop, collect.

void synthetic_cases() {
    // --- pure tables -------------------------------------------------------
    CHECK_EQ(std::string(gm::thinganim::clip_name(ThingClip::none)), "none");
    CHECK_EQ(std::string(gm::thinganim::clip_name(ThingClip::idle)), "idle");
    CHECK_EQ(std::string(gm::thinganim::clip_name(ThingClip::walk)), "walk");
    CHECK_EQ(std::string(gm::thinganim::clip_name(ThingClip::attack)), "attack");
    CHECK_EQ(std::string(gm::thinganim::clip_name(ThingClip::death)), "death");
    CHECK_EQ(gm::thinganim::clip_suffix(ThingClip::none), "");
    CHECK_EQ(gm::thinganim::clip_suffix(ThingClip::idle), "_idle");
    CHECK_EQ(gm::thinganim::clip_suffix(ThingClip::walk), "_walk");
    CHECK_EQ(gm::thinganim::clip_suffix(ThingClip::attack), "_atk");
    CHECK_EQ(gm::thinganim::clip_suffix(ThingClip::death), "_dead");

    CHECK(gm::thinganim::clip_for_state(AiState::idle) == ThingClip::idle);
    CHECK(gm::thinganim::clip_for_state(AiState::patrol) == ThingClip::walk);
    CHECK(gm::thinganim::clip_for_state(AiState::alert) == ThingClip::walk);
    CHECK(gm::thinganim::clip_for_state(AiState::attack) == ThingClip::attack);
    CHECK(gm::thinganim::clip_for_state(AiState::dead) == ThingClip::death);
    CHECK(!gm::thinganim::clip_for_state(static_cast<AiState>(200)).has_value());

    // --- a temporary installation -----------------------------------------
    const fs::path root = fs::temp_directory_path() / "gm_thing_anim_synthetic";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Sobanim", ec);
    CHECK(!ec);
    // mode 1, two frames of 10 ticks -> total 20, wraps.
    write_text(root / "Sobanim" / "loop.soo", "1\n2\na 10\nb 10\n");
    // mode 0, same frames -> total 20, holds the last frame.
    write_text(root / "Sobanim" / "hold.soo", "0\n2\na 10\nb 10\n");
    // No frames at all: parse succeeds, the timeline refuses.
    write_text(root / "Sobanim" / "empty.soo", "0\n0\n");
    // mode 7: the engine has no normalisation for it (docs/SOO.md §3.1).
    write_text(root / "Sobanim" / "badmode.soo", "7\n2\nf1 10\nf2 10\n");
    // Zero total duration: 0x433e67 returns without a pose.
    write_text(root / "Sobanim" / "zero.soo", "0\n2\nf1 0\nf2 0\n");
    // ASCII case-insensitive lookup: the file is upper-case.
    write_text(root / "Sobanim" / "CASE.SOO", "0\n1\nonly 5\n");
    // The state-specific variant of `thing`; the base `thing.soo` is the
    // fallback for every other state.
    write_text(root / "Sobanim" / "thing.soo", "1\n1\nbase 10\n");
    write_text(root / "Sobanim" / "thing_atk.soo", "0\n2\natk1 10\natk2 10\n");

    ThingAnimConfig config;
    config.root = root;

    // --- hold vs loop on the same frames ----------------------------------
    const auto loop0 = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, 0,
                                                      config);
    CHECK(loop0.has_value());
    if (loop0) {
        CHECK_EQ(loop0->clip, ThingClip::idle);
        CHECK_EQ(loop0->clip_name, std::string("loop"));
        CHECK_EQ(loop0->frame, 0);
        CHECK_EQ(loop0->tick, 0);
        CHECK(loop0->looping);
    }
    const auto loop19 = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, 19,
                                                       config);
    CHECK(loop19.has_value());
    if (loop19) {
        CHECK_EQ(loop19->frame, 1);
        CHECK_EQ(loop19->tick, 19);
        CHECK(loop19->looping);
    }
    const auto loop20 = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, 20,
                                                       config);
    CHECK(loop20.has_value());
    if (loop20) {
        CHECK_EQ(loop20->frame, 0); // 20 % 20 == 0: wraps to the first frame
        CHECK_EQ(loop20->tick, 0);
        CHECK(loop20->looping);
    }
    const auto loop21 = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, 21,
                                                       config);
    CHECK(loop21.has_value());
    if (loop21) {
        CHECK_EQ(loop21->frame, 0);
        CHECK_EQ(loop21->tick, 1);
    }

    const auto hold19 = gm::thinganim::bind_thing_anim(model_thing("hold"), AiState::idle, 19,
                                                       config);
    CHECK(hold19.has_value());
    if (hold19) {
        CHECK_EQ(hold19->frame, 1);
        CHECK_EQ(hold19->tick, 19);
        CHECK(!hold19->looping);
    }
    const auto hold20 = gm::thinganim::bind_thing_anim(model_thing("hold"), AiState::idle, 20,
                                                       config);
    CHECK(hold20.has_value());
    if (hold20) {
        CHECK_EQ(hold20->frame, 1); // clamped to total-1 = 19 -> last frame
        CHECK_EQ(hold20->tick, 19);
        CHECK(!hold20->looping);
    }
    const auto hold10k = gm::thinganim::bind_thing_anim(model_thing("hold"), AiState::idle, 10000,
                                                        config);
    CHECK(hold10k.has_value());
    if (hold10k) {
        CHECK_EQ(hold10k->frame, 1);
        CHECK_EQ(hold10k->tick, 19);
    }

    // Death holds a looping clip on its last frame (the model knob), and the
    // same call with `hold_on_death = false` loops.
    const auto death_hold = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::dead, 25,
                                                           config);
    CHECK(death_hold.has_value());
    if (death_hold) {
        CHECK_EQ(death_hold->clip, ThingClip::death);
        CHECK_EQ(death_hold->frame, 1);
        CHECK_EQ(death_hold->tick, 19);
        CHECK(!death_hold->looping);
    }
    ThingAnimConfig no_hold = config;
    no_hold.hold_on_death = false;
    const auto death_loop = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::dead, 25,
                                                           no_hold);
    CHECK(death_loop.has_value());
    if (death_loop) {
        CHECK_EQ(death_loop->frame, 0); // 25 % 20 == 5 -> frame 0
        CHECK_EQ(death_loop->tick, 5);
        CHECK(death_loop->looping);
    }

    // --- probe order and ASCII case ---------------------------------------
    const auto attack = gm::thinganim::bind_thing_anim(model_thing("thing"), AiState::attack, 0,
                                                       config);
    CHECK(attack.has_value());
    if (attack) {
        CHECK_EQ(attack->clip_name, std::string("thing_atk")); // the variant won
        CHECK_EQ(attack->frame, 0);
    }
    const auto idle = gm::thinganim::bind_thing_anim(model_thing("thing"), AiState::idle, 0,
                                                     config);
    CHECK(idle.has_value());
    if (idle) {
        CHECK_EQ(idle->clip_name, std::string("thing")); // no _idle variant -> base
        CHECK_EQ(idle->frame, 0);
    }
    const auto upper = gm::thinganim::bind_thing_anim(model_thing("case"), AiState::patrol, 0,
                                                      config);
    CHECK(upper.has_value());
    if (upper) {
        CHECK_EQ(upper->clip_name, std::string("CASE"));
        CHECK_EQ(upper->frame, 0);
    }

    // --- refusals ----------------------------------------------------------
    std::string error;
    CHECK(!gm::thinganim::bind_thing_anim(Thing{}, AiState::idle, 0, config, &error));
    CHECK(starts_with(error, "thing_anim:"));
    CHECK(has(error, "no model"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, -1, config, &error));
    CHECK(starts_with(error, "thing_anim:"));
    CHECK(has(error, "negative tick"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("loop"), static_cast<AiState>(200), 0,
                                          config, &error));
    CHECK(has(error, "unknown AI state"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("../loop"), AiState::idle, 0, config,
                                          &error));
    CHECK(has(error, "not a plain"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("nosuch"), AiState::idle, 0, config,
                                          &error));
    CHECK(starts_with(error, "thing_anim:"));
    CHECK(has(error, "no clip"));
    CHECK(has(error, "nosuch_idle.soo")); // the probe is visible in the message
    CHECK(has(error, "nosuch.soo"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("empty"), AiState::idle, 0, config,
                                          &error));
    CHECK(has(error, "no frames"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("badmode"), AiState::idle, 0, config,
                                          &error));
    CHECK(has(error, "unsupported .soo mode 7"));

    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("zero"), AiState::idle, 0, config, &error));
    CHECK(has(error, "duration is not positive"));

    // A root without a `Sobanim/` directory and no candidate installation.
    ThingAnimConfig none;
    none.root = fs::temp_directory_path() / "gm_thing_anim_no_such_root";
    fs::remove_all(none.root, ec);
    error.clear();
    CHECK(!gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, 0, none, &error));
    CHECK(has(error, "no .soo clips"));

    // A success clears the error text.
    error = "stale";
    const auto ok = gm::thinganim::bind_thing_anim(model_thing("loop"), AiState::idle, 0, config,
                                                   &error);
    CHECK(ok.has_value());
    CHECK(error.empty());

    // --- collect -----------------------------------------------------------
    std::vector<Thing> things{model_thing("loop"), model_thing("hold"), model_thing("nosuch")};
    std::vector<AiState> states{AiState::idle, AiState::idle, AiState::dead};
    std::string first_error;
    const ThingAnimStats stats = gm::thinganim::collect(things, states, 0, config, &first_error);
    CHECK_EQ(stats.bound, 2);
    CHECK_EQ(stats.unbound, 1);
    CHECK_EQ(stats.animated, 2); // loop and hold both still inside their first frame
    CHECK_EQ(stats.still, 0);
    CHECK(has(first_error, "thing_anim:"));
    CHECK(has(first_error, "no clip"));

    const ThingAnimStats late = gm::thinganim::collect(things, states, 19, config);
    CHECK_EQ(late.bound, 2);
    CHECK_EQ(late.animated, 1); // the loop wraps, the mode 0 clip is on its last tick
    CHECK_EQ(late.still, 1);

    const ThingAnimStats frozen = gm::thinganim::collect(things, states, 20, config);
    CHECK_EQ(frozen.animated, 1); // loop is still looping ...
    CHECK_EQ(frozen.still, 1);    // ... the held clip stays frozen

    // Fewer states than things: the tail is unbound, not silently idle.
    const ThingAnimStats short_states =
        gm::thinganim::collect(things, std::span<const AiState>(states).first(1), 0, config);
    CHECK_EQ(short_states.bound, 1);
    CHECK_EQ(short_states.unbound, 2);

    const ThingAnimStats negative = gm::thinganim::collect(things, states, -1, config);
    CHECK_EQ(negative.bound, 0);
    CHECK_EQ(negative.unbound, 3);

    // Determinism over the synthetic root.
    const ThingAnimStats again = gm::thinganim::collect(things, states, 0, config);
    CHECK_EQ(again.bound, stats.bound);
    CHECK_EQ(again.unbound, stats.unbound);
    CHECK_EQ(again.animated, stats.animated);
    CHECK_EQ(again.still, stats.still);

    gm::thinganim::clear_clip_cache();
    std::printf("synthetic: loop/hold 20 ticks, probe order, 9 refusals, collect ok\n");
}

// ---------------------------------------------------------------------------
// Real: the two proven timelines (mode 0 hold, mode 1 loop).

void timeline_cases(const fs::path& game) {
    const ThingAnimConfig config; // default root resolution
    CHECK_EQ(gm::thinganim::resolve_anim_root(), game);

    // kndeath.soo: mode 0, 4 frames x 15 ticks = 60, names kndeath1..4.
    const Thing kndeath = model_thing("kndeath");
    const auto k0 = gm::thinganim::bind_thing_anim(kndeath, AiState::idle, 0, config);
    CHECK(k0.has_value());
    if (k0) {
        CHECK_EQ(k0->clip_name, std::string("kndeath"));
        CHECK_EQ(k0->frame, 0);
        CHECK_EQ(k0->tick, 0);
        CHECK(!k0->looping);
    }
    const auto k59 = gm::thinganim::bind_thing_anim(kndeath, AiState::idle, 59, config);
    CHECK(k59.has_value());
    if (k59) {
        CHECK_EQ(k59->frame, 3);
        CHECK_EQ(k59->tick, 59);
    }
    const auto k60 = gm::thinganim::bind_thing_anim(kndeath, AiState::idle, 60, config);
    CHECK(k60.has_value());
    if (k60) {
        CHECK_EQ(k60->frame, 3); // total-1 = 59 -> held on kndeath4
        CHECK_EQ(k60->tick, 59);
        CHECK(!k60->looping);
    }
    const auto k61 = gm::thinganim::bind_thing_anim(kndeath, AiState::idle, 61, config);
    CHECK(k61.has_value());
    if (k61) {
        CHECK_EQ(k61->frame, 3);
        CHECK_EQ(k61->tick, 59);
    }
    const auto kdead = gm::thinganim::bind_thing_anim(kndeath, AiState::dead, 60, config);
    CHECK(kdead.has_value());
    if (kdead) {
        CHECK_EQ(kdead->frame, 3);
        CHECK(!kdead->looping);
    }

    // kntbox.soo: mode 1, 10 frames x 20 ticks = 200.
    const Thing kntbox = model_thing("kntbox");
    const auto t0 = gm::thinganim::bind_thing_anim(kntbox, AiState::idle, 0, config);
    CHECK(t0.has_value());
    if (t0) {
        CHECK_EQ(t0->clip_name, std::string("kntbox"));
        CHECK_EQ(t0->frame, 0);
        CHECK_EQ(t0->tick, 0);
        CHECK(t0->looping);
    }
    const auto t199 = gm::thinganim::bind_thing_anim(kntbox, AiState::idle, 199, config);
    CHECK(t199.has_value());
    if (t199) {
        CHECK_EQ(t199->frame, 9);
        CHECK_EQ(t199->tick, 199);
    }
    const auto t200 = gm::thinganim::bind_thing_anim(kntbox, AiState::idle, 200, config);
    CHECK(t200.has_value());
    if (t200) {
        CHECK_EQ(t200->frame, 0); // 200 % 200 == 0: the loop restarts
        CHECK_EQ(t200->tick, 0);
        CHECK(t200->looping);
    }
    const auto t201 = gm::thinganim::bind_thing_anim(kntbox, AiState::idle, 201, config);
    CHECK(t201.has_value());
    if (t201) {
        CHECK_EQ(t201->frame, 0);
        CHECK_EQ(t201->tick, 1);
    }
    const auto t2020 = gm::thinganim::bind_thing_anim(kntbox, AiState::idle, 2020, config);
    CHECK(t2020.has_value());
    if (t2020) {
        CHECK_EQ(t2020->frame, 1); // 2020 % 200 == 20 -> frame 1
        CHECK_EQ(t2020->tick, 20);
    }
    // Death on the same loop clip: held instead of wrapped.
    const auto tdead = gm::thinganim::bind_thing_anim(kntbox, AiState::dead, 250, config);
    CHECK(tdead.has_value());
    if (tdead) {
        CHECK_EQ(tdead->frame, 9);
        CHECK_EQ(tdead->tick, 199);
        CHECK(!tdead->looping);
    }
    ThingAnimConfig no_hold;
    no_hold.hold_on_death = false;
    const auto tdead_loop = gm::thinganim::bind_thing_anim(kntbox, AiState::dead, 250, no_hold);
    CHECK(tdead_loop.has_value());
    if (tdead_loop) {
        CHECK_EQ(tdead_loop->frame, 2); // 250 % 200 == 50 -> frame 2
        CHECK_EQ(tdead_loop->tick, 50);
        CHECK(tdead_loop->looping);
    }

    std::printf("timeline: kndeath(mode 0,4x15): t0 f%d t%d, t60 f%d t%d, t61 f%d; "
                "kntbox(mode 1,10x20): t0 f%d, t199 f%d, t200 f%d t%d, t201 t%d, "
                "t2020 f%d; dead t250 f%d held\n",
                k0 ? k0->frame : -1, k0 ? k0->tick : -1, k60 ? k60->frame : -1,
                k60 ? k60->tick : -1, k61 ? k61->frame : -1, t0 ? t0->frame : -1,
                t199 ? t199->frame : -1, t200 ? t200->frame : -1, t200 ? t200->tick : -1,
                t201 ? t201->tick : -1, t2020 ? t2020->frame : -1, tdead ? tdead->frame : -1);
}

// ---------------------------------------------------------------------------
// Real: a type of level13 whose model is a clip.

gm::SbtLevel load_level(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = read_file(path);
    std::string error;
    auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        std::fprintf(stderr, "thing_anim: parse_sbt(%s): %s\n", path.string().c_str(),
                     error.c_str());
        return {};
    }
    return *level;
}

gm::Things load_things(const gm::SbtLevel& level) {
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    return gm::Things::load(level, gm::vm_state_from_level(level), options);
}

void real_type_cases(const fs::path& game) {
    const fs::path path = game / "level13.sbt";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        std::printf("thing_anim: level13.sbt not installed, type cases skipped\n");
        return;
    }
    const gm::SbtLevel level = load_level(path);
    if (level.records.empty() || level.sobs.empty()) {
        CHECK(false);
        return;
    }
    const gm::Things things = load_things(level);
    CHECK(things.ok());
    const gm::ThingTypeTable& types = things.type_table();
    // `models` is the level's SOB_TABLE (94 names), `model_indices` the
    // `THING_TYPES +0x1a4` index per type; -1 is the engine's "none".
    const auto model_of = [&types](std::size_t i) -> std::string {
        const int index = i < types.model_indices.size() ? types.model_indices[i] : -1;
        if (index < 0 || static_cast<std::size_t>(index) >= types.models.size()) {
            return {};
        }
        return types.models[static_cast<std::size_t>(index)];
    };

    int cultist = -1;
    for (std::size_t i = 0; i < types.names.size(); ++i) {
        if (types.names[i] == "CULTIST") {
            cultist = static_cast<int>(i);
        }
    }
    CHECK(cultist >= 0);
    if (cultist < 0) {
        return;
    }
    CHECK_EQ(model_of(static_cast<std::size_t>(cultist)), std::string("CULTIST"));

    Thing thing;
    thing.id = 1;
    thing.type = cultist;
    thing.type_name = types.names[static_cast<std::size_t>(cultist)];
    thing.model = model_of(static_cast<std::size_t>(cultist));

    // Cultist.soo: mode 1, four frames (50/150/50/150) = 400 ticks.
    const ThingAnimConfig config;
    const auto idle = gm::thinganim::bind_thing_anim(thing, AiState::idle, 210, config);
    CHECK(idle.has_value());
    if (idle) {
        CHECK_EQ(idle->clip, ThingClip::idle);
        CHECK_EQ(idle->clip_name, std::string("Cultist"));
        CHECK_EQ(idle->frame, 2); // 50 + 150 = 200, 210 is 10 into frame 2
        CHECK_EQ(idle->tick, 210);
        CHECK(idle->looping);
    }
    const auto walk = gm::thinganim::bind_thing_anim(thing, AiState::alert, 210, config);
    CHECK(walk.has_value());
    if (walk) {
        CHECK_EQ(walk->clip, ThingClip::walk); // alert shares the walk clip
        CHECK_EQ(walk->clip_name, std::string("Cultist"));
        CHECK_EQ(walk->frame, 2);
        CHECK(walk->looping);
    }
    const auto attack = gm::thinganim::bind_thing_anim(thing, AiState::attack, 210, config);
    CHECK(attack.has_value());
    if (attack) {
        CHECK_EQ(attack->clip, ThingClip::attack);
        CHECK_EQ(attack->clip_name, std::string("Cultist")); // no Cultist_atk.soo
        CHECK_EQ(attack->frame, 2);
        CHECK(attack->looping);
    }
    const auto dead = gm::thinganim::bind_thing_anim(thing, AiState::dead, 401, config);
    CHECK(dead.has_value());
    if (dead) {
        CHECK_EQ(dead->clip, ThingClip::death);
        CHECK_EQ(dead->frame, 3); // held on the last frame
        CHECK_EQ(dead->tick, 399);
        CHECK(!dead->looping);
    }
    const auto dead0 = gm::thinganim::bind_thing_anim(thing, AiState::dead, 0, config);
    CHECK(dead0.has_value());
    if (dead0) {
        CHECK_EQ(dead0->frame, 0);
        CHECK_EQ(dead0->tick, 0);
        CHECK(!dead0->looping);
    }

    // How many types of the level point at a clip at all (level13: CULTIST ->
    // Cultist.soo and CultKC -> slump.soo).
    int typed_clips = 0;
    std::string chained;
    for (std::size_t i = 0; i < types.names.size(); ++i) {
        const std::string model = model_of(i);
        if (model.empty()) {
            continue;
        }
        Thing candidate;
        candidate.id = static_cast<int>(i);
        candidate.type = static_cast<int>(i);
        candidate.type_name = types.names[i];
        candidate.model = model;
        if (gm::thinganim::bind_thing_anim(candidate, AiState::idle, 0, config)) {
            ++typed_clips;
            chained += types.names[i] + "->" + model + " ";
        }
    }
    CHECK_EQ(typed_clips, 5);

    // The runtime instances of the level: how many bind, and the animated /
    // still split (every agent is idle here; the AI state is an external
    // input, see docs/THING_ANIM_BIND.md §4).  Tick 0 shows every clip at its
    // start; tick 2000 is past every clip's total, so the mode 0 clips are
    // frozen while the mode 1 ones are still moving.
    const auto& runtime = things.things();
    std::vector<AiState> idle_states(runtime.size(), AiState::idle);
    const ThingAnimStats stats = gm::thinganim::collect(runtime, idle_states, 0, config);
    const ThingAnimStats late = gm::thinganim::collect(runtime, idle_states, 2000, config);
    CHECK_EQ(stats.bound + stats.unbound, static_cast<int>(runtime.size()));
    CHECK_EQ(stats.animated + stats.still, stats.bound);
    CHECK_EQ(late.bound, stats.bound);
    CHECK_EQ(late.animated + late.still, late.bound);

    std::printf("level13: types=%zu clips=%d [%s] things=%zu bound=%d unbound=%d "
                "t0(animated=%d still=%d) t2000(animated=%d still=%d); "
                "CULTIST/Cultist.soo t210 frame=%d, dead t401 frame=%d tick=%d\n",
                types.names.size(), typed_clips, chained.c_str(), runtime.size(), stats.bound,
                stats.unbound, stats.animated, stats.still, late.animated, late.still,
                idle ? idle->frame : -1, dead ? dead->frame : -1, dead ? dead->tick : -1);
}

// ---------------------------------------------------------------------------
// Real: the whole shipped clip corpus and the "no clip" case.

struct ClipInfo {
    std::string stem;   // as spelled on disk
    int mode = 0;
    int declared = 0;
    int frames = 0;     // frames the engine would load
    int total = 0;      // ticks
};

/// Lower-case extension check (the corpus has `DISRUPT.SOO`, `NUKEring.SOO`).
bool is_soo_file(const std::string& name) {
    if (name.size() < 4) {
        return false;
    }
    std::string ext = name.substr(name.size() - 4);
    for (char& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext == ".soo";
}

std::vector<std::string> scan_clip_stems(const fs::path& game) {
    std::vector<std::string> stems;
    std::error_code ec;
    fs::directory_iterator it(game / "Sobanim", ec);
    if (ec) {
        return stems;
    }
    for (const auto& entry : it) {
        std::error_code ec2;
        if (!entry.is_regular_file(ec2)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!is_soo_file(name)) {
            continue;
        }
        stems.push_back(name.substr(0, name.size() - 4));
    }
    std::sort(stems.begin(), stems.end());
    return stems;
}

void corpus_case(const fs::path& game) {
    const std::vector<std::string> stems = scan_clip_stems(game);
    CHECK_EQ(stems.size(), 40u);

    const ThingAnimConfig config;
    const std::vector<AiState> states{AiState::idle, AiState::patrol, AiState::attack,
                                      AiState::dead};
    const std::vector<int> ticks{0, 1, 57, 199, 1000};

    int bound = 0;
    int unbound = 0;
    int variant_hits = 0;
    int mode0 = 0;
    int mode1 = 0;
    int frame_checks = 0;
    int mask_checks = 0;
    std::vector<ClipInfo> clips;

    for (const std::string& stem : stems) {
        std::string error;
        const fs::path path = game / "Sobanim" / stem;
        // The parse below needs the real file name; the stem keeps the case of
        // the directory entry, so `<stem>.soo`/`<stem>.SOO` both resolve here.
        std::error_code ec;
        fs::path file = path;
        file += ".soo";
        if (!fs::is_regular_file(file, ec)) {
            file = path;
            file += ".SOO";
        }
        const auto soo = gm::parse_soo(read_file(file), error);
        if (!soo) {
            std::fprintf(stderr, "thing_anim: parse_soo(%s): %s\n", file.string().c_str(),
                         error.c_str());
            CHECK(false);
            continue;
        }
        ClipInfo info;
        info.stem = stem;
        info.mode = soo->mode;
        info.declared = soo->declared;
        info.frames = soo->declared > 0
                          ? std::min<int>(soo->declared, static_cast<int>(soo->frames.size()))
                          : 0;
        for (int i = 0; i < info.frames; ++i) {
            info.total += soo->frames[static_cast<std::size_t>(i)].duration;
        }
        clips.push_back(info);
        if (info.mode == 0) {
            ++mode0;
        } else if (info.mode == 1) {
            ++mode1;
        }
        // The loader only reaches `.soo` when no same-named `.sob` exists.
        fs::path sob = game / "SOBS" / stem;
        sob += ".sob";
        if (fs::is_regular_file(sob, ec)) {
            std::fprintf(stderr, "thing_anim: %s.soo is shadowed by a .sob\n", stem.c_str());
            CHECK(false);
        }

        // No state-specific variant is shipped: every binding must report the
        // base clip, never `<stem>_idle`/`_walk`/`_atk`/`_dead`.
        for (AiState state : states) {
            for (int tick : ticks) {
                std::string bind_error;
                const auto binding =
                    gm::thinganim::bind_thing_anim(model_thing(stem), state, tick, config,
                                                   &bind_error);
                if (!binding) {
                    ++unbound;
                    std::fprintf(stderr, "thing_anim: %s/%d/%d: %s\n", stem.c_str(),
                                 static_cast<int>(state), tick, bind_error.c_str());
                    CHECK(false);
                    continue;
                }
                ++bound;
                if (binding->clip_name != stem) {
                    ++variant_hits;
                }
                CHECK(binding->frame >= 0);
                CHECK(binding->frame < info.frames);
                CHECK(binding->tick >= 0);
                CHECK(binding->tick < info.total);
                if (binding->looping == (info.mode == 1 && state != AiState::dead)) {
                    ++mask_checks;
                } else {
                    std::fprintf(stderr, "thing_anim: %s state=%d looping=%d mode=%d\n",
                                 stem.c_str(), static_cast<int>(state), binding->looping ? 1 : 0,
                                 info.mode);
                    CHECK(false);
                }
                ++frame_checks;
            }
        }
    }

    CHECK_EQ(unbound, 0);
    CHECK_EQ(bound, 800); // 40 clips x 4 states x 5 ticks
    CHECK_EQ(variant_hits, 0);
    CHECK_EQ(mode0, 26);
    CHECK_EQ(mode1, 14);
    CHECK_EQ(mode0 + mode1, 40);
    CHECK_EQ(frame_checks, bound);
    CHECK_EQ(mask_checks, bound);

    // Determinism: an identical second pass must return the same bindings.
    bool identical = true;
    for (const std::string& stem : stems) {
        for (AiState state : states) {
            for (int tick : ticks) {
                const auto a = gm::thinganim::bind_thing_anim(model_thing(stem), state, tick,
                                                             config);
                const auto b = gm::thinganim::bind_thing_anim(model_thing(stem), state, tick,
                                                             config);
                identical = identical && same_binding(a, b);
            }
        }
    }
    CHECK(identical);

    // A static model of dm1 (`.sob` present, no `.soo`): the clip refusal.
    const gm::SbtLevel level = load_level(game / "dm1.sbt");
    std::string no_clip_model;
    for (const gm::SbtSob& sob : level.sobs) {
        if (sob.name.empty()) {
            continue;
        }
        if (!gm::thinganim::bind_thing_anim(model_thing(sob.name), AiState::idle, 0, config)) {
            no_clip_model = sob.name;
            break;
        }
    }
    CHECK(!no_clip_model.empty());
    std::string error;
    CHECK(!gm::thinganim::bind_thing_anim(model_thing(no_clip_model), AiState::idle, 0, config,
                                          &error));
    CHECK(starts_with(error, "thing_anim:"));
    CHECK(has(error, "no clip"));

    // How many SOB_TABLE models of dm1 are clips (3: FMBLOK, TELEPIN, BAMFRAYS).
    int model_clips = 0;
    for (const gm::SbtSob& sob : level.sobs) {
        Thing candidate = model_thing(sob.name);
        if (gm::thinganim::bind_thing_anim(candidate, AiState::idle, 0, config)) {
            ++model_clips;
        }
    }
    CHECK_EQ(model_clips, 10);

    std::vector<std::string> static_stems;
    {
        std::error_code dir_ec;
        fs::directory_iterator it(game / "SOBS", dir_ec);
        if (!dir_ec) {
            for (const auto& entry : it) {
                std::error_code file_ec;
                if (!entry.is_regular_file(file_ec)) {
                    continue;
                }
                std::string name = entry.path().filename().string();
                if (name.size() < 5) {
                    continue;
                }
                std::string ext = name.substr(name.size() - 4);
                for (char& ch : ext) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (ext == ".sob") {
                    static_stems.push_back(name.substr(0, name.size() - 4));
                }
            }
        }
    }
    std::sort(static_stems.begin(), static_stems.end());
    int static_clip = 0;
    for (const std::string& stem : static_stems) {
        if (gm::thinganim::bind_thing_anim(model_thing(stem), AiState::idle, 0, config)) {
            ++static_clip;
        }
    }
    CHECK_EQ(static_stems.size(), 672u);
    CHECK_EQ(static_clip, 0); // every .soo has no same-named .sob: 0 overlap

    int no_model = 0;
    for (std::size_t i = 0; i < stems.size(); ++i) {
        if (!gm::thinganim::bind_thing_anim(Thing{}, AiState::idle, 0, config)) {
            ++no_model;
        }
    }
    CHECK_EQ(no_model, 40);

    int frame_total = 0;
    for (const ClipInfo& info : clips) {
        frame_total += info.frames;
    }
    std::printf("corpus: files=%zu bound=%d unbound=%d variant_hits=%d mode0=%d mode1=%d "
                "frames=%d no_model=%d; static .sob models=%zu bound_as_clip=%d; "
                "dm1 sobs=%zu clip_models=%d static_refusal=%s\n",
                clips.size(), bound, unbound, variant_hits, mode0, mode1, frame_total,
                no_model, static_stems.size(), static_clip, level.sobs.size(), model_clips,
                no_clip_model.c_str());
}

} // namespace

int main() {
    synthetic_cases();

    const auto game = find_game_dir();
    if (!game) {
        std::printf("thing_anim: game directory not found, synthetic cases only\n");
    } else {
        std::printf("thing_anim: game=%s\n", game->string().c_str());
        timeline_cases(*game);
        real_type_cases(*game);
        corpus_case(*game);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "thing_anim: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("thing_anim: all checks passed\n");
    return 0;
}
