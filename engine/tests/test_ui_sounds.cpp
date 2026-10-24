// UI sound triggers (ROADMAP M8.6, slice 2, docs/UI_SOUNDS.md).
//
// The doc pins the six samples a trigger can name and their `sounds/SOU.IDX`
// record indices (§1.1/§1.2).  This test checks them against the shipped data:
//
//   * `SOUND_SAMPLE_TABLE` of `level0` (or `--level NAME`) carries every sample
//     with `count > 0` and the same name in the slot;
//   * the archive really has a record at the documented 0-based index and the
//     record name matches (BigFile order, `sounds/SOU.IDX`), with a non-zero
//     payload;
//   * the trigger table: Up/Down only when the selection moved, Enter ->
//     `select` + `selectm` (the doc proves both on *every* Enter, so a toggle
//     row is not distinguished), Left/Right -> `select` by default with the
//     SOUND OPTIONS / GRAPHICS OPTIONS overrides, Esc and screen 16 silent;
//   * `ui_sounds_for` on a copy of `FrontendMenu`: the caller's menu is not
//     touched, `down` on MAIN -> `updown`, `enter` -> `select`/`selectm` and
//     (model) `menuon` when the screen changes; `vback` is never emitted;
//   * negatives: unknown keys, empty table, out-of-range indices, no crash on
//     `None`/`Exit`, no game installation.
//
// Numbers are printed so docs/UI_SOUNDS_STATE.md can quote them.

#include "game_path.h"
#include "res/bigfile.h"
#include "res/sbt.h"
#include "ui/frontend_menu.h"
#include "ui/ui_sounds.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

gm::KeyEvent key(gm::KeyEvent::Kind kind) {
    gm::KeyEvent event;
    event.kind = kind;
    return event;
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

bool has(const gm::UiSoundPlan& plan, gm::UiSound sound) {
    for (const gm::UiSoundEvent& event : plan.events) {
        if (event.sound == sound) {
            return true;
        }
    }
    return false;
}

std::string plan_names(const gm::UiSoundPlan& plan) {
    std::string out;
    for (const gm::UiSoundEvent& event : plan.events) {
        if (!out.empty()) {
            out += ' ';
        }
        out += std::string(event.name);
    }
    return out.empty() ? "-" : out;
}

/// A: the sample table of the doc (§1.1/§1.2) and its real rows.
void test_samples() {
    const auto samples = gm::ui_sound_samples();
    CHECK(samples.size() == 6);

    struct Expect {
        gm::UiSound sound;
        const char* name;
        int index;
    };
    const std::array<Expect, 6> expected = {{
        {gm::UiSound::up_down, "updown", 581},
        {gm::UiSound::select, "select", 510},
        {gm::UiSound::select_m, "selectm", 511},
        {gm::UiSound::menu_on, "menuon", 425},
        {gm::UiSound::v_left, "vleft", 804},
        {gm::UiSound::back, "vback", 636},
    }};
    for (const Expect& row : expected) {
        const gm::UiSoundSample* sample = gm::ui_sound_sample(row.sound);
        CHECK(sample != nullptr);
        if (!sample) {
            continue;
        }
        CHECK(sample->name == std::string_view(row.name));
        CHECK(sample->table_index == row.index);
    }

    // Bounds: every enum value exists, out-of-range values do not.
    CHECK(gm::ui_sound_sample(static_cast<gm::UiSound>(-1)) == nullptr);
    CHECK(gm::ui_sound_sample(static_cast<gm::UiSound>(99)) == nullptr);
}

/// B: the level's `SOUND_SAMPLE_TABLE` + the archive order (real install).
struct RealRow {
    std::string name;
    int index = -1;
    int slot = -1;
    std::uint32_t count = 0;
    std::uint64_t archive_size = 0;
    std::string archive_name;
};

std::vector<RealRow> check_real(const fs::path& game, const gm::SoundSampleTable& table) {
    std::vector<RealRow> rows;

    std::string error;
    const auto big = gm::BigFile::open(game / "sounds" / "SOU.IDX", error);
    CHECK(big.has_value());
    if (!big) {
        std::fprintf(stderr, "ui_sounds: %s\n", error.c_str());
        return rows;
    }

    for (const gm::UiSoundSample& sample : gm::ui_sound_samples()) {
        RealRow row;
        row.name = std::string(sample.name);
        row.index = sample.table_index;

        // Level table: the slot with this name must be active (`count > 0`).
        CHECK(gm::table_has(table, sample.table_index));
        row.slot = gm::table_slot(table, sample.name);
        CHECK(row.slot >= 0);
        if (row.slot >= 0) {
            const auto slot = static_cast<std::size_t>(row.slot);
            row.count = table.counts[slot];
            CHECK(row.count > 0);
            CHECK(lower(table.names[slot]) == lower(row.name));
        }

        // Archive: the documented 0-based record index must hold this name and
        // a non-empty payload (`sounds/SOU.DAT` slice of the next offset).
        const auto& entries = big->entries();
        CHECK(row.index >= 0 && static_cast<std::size_t>(row.index) < entries.size());
        if (row.index >= 0 && static_cast<std::size_t>(row.index) < entries.size()) {
            const auto index = static_cast<std::size_t>(row.index);
            const gm::BigEntry& entry = entries[index];
            row.archive_name = entry.name;
            CHECK(lower(entry.name) == lower(row.name));
            const std::uint64_t start = entry.offset;
            const std::uint64_t end =
                index + 1 < entries.size() ? entries[index + 1].offset : big->dat_size();
            row.archive_size = end > start ? end - start : 0;
            CHECK(row.archive_size > 0);
            CHECK(big->dat_size() > start);
        }
        rows.push_back(row);
    }
    return rows;
}

/// C: `ui_sound_for` - one event per key, no state machine.
void test_triggers() {
    // Up/Down: only a real move.
    CHECK(!gm::ui_sound_for(key(gm::KeyEvent::Up), gm::Screen::Main, gm::Screen::Main,
                            false)
               .has_value());
    CHECK(!gm::ui_sound_for(key(gm::KeyEvent::Down), gm::Screen::Main,
                            gm::Screen::Main, false)
               .has_value());
    const auto up = gm::ui_sound_for(key(gm::KeyEvent::Up), gm::Screen::Main,
                                     gm::Screen::Main, true);
    CHECK(up.has_value());
    if (up) {
        CHECK(up->sound == gm::UiSound::up_down);
        CHECK(up->table_index == 581);
        CHECK(up->name == "updown");
    }
    const auto down = gm::ui_sound_for(key(gm::KeyEvent::Down),
                                       gm::Screen::SoundOptions,
                                       gm::Screen::SoundOptions, true);
    CHECK(down && down->sound == gm::UiSound::up_down);

    // Enter: select on an ordinary screen; silence on KEY JAMMING (screen 16)
    // and on the intro (id 0, not in 1..37).
    const auto enter = gm::ui_sound_for(key(gm::KeyEvent::Enter), gm::Screen::Main,
                                        gm::Screen::SoundOptions, false);
    CHECK(enter && enter->sound == gm::UiSound::select && enter->table_index == 510);
    CHECK(!gm::ui_sound_for(key(gm::KeyEvent::Enter), gm::Screen::KeyJamming,
                            gm::Screen::KeyJamming, false)
               .has_value());
    CHECK(!gm::ui_sound_for(key(gm::KeyEvent::Enter), gm::Screen::Intro,
                            gm::Screen::Main, false)
               .has_value());

    // Left/Right: the proved default is `select` (event 2).  vleft is item
    // specific and reachable through ui_sounds_for below.
    const auto left = gm::ui_sound_for(key(gm::KeyEvent::Left), gm::Screen::Main,
                                       gm::Screen::Main, true);
    CHECK(left && left->sound == gm::UiSound::select);
    CHECK(!gm::ui_sound_for(key(gm::KeyEvent::Right), gm::Screen::KeyJamming,
                            gm::Screen::KeyJamming, false)
               .has_value());

    // Esc: the doc proves silence on every screen.
    for (gm::Screen screen : gm::FrontendMenu::screens()) {
        CHECK(!gm::ui_sound_for(key(gm::KeyEvent::Escape), screen, screen, false)
                   .has_value());
    }

    // Unknown keys are silent everywhere.
    for (gm::KeyEvent::Kind kind :
         {gm::KeyEvent::Backspace, gm::KeyEvent::Text, gm::KeyEvent::Other}) {
        CHECK(!gm::ui_sound_for(key(kind), gm::Screen::Main, gm::Screen::Main, true)
                   .has_value());
    }
}

/// D: `ui_sounds_for` on a copy of the automaton.
void test_plan() {
    // down on MAIN: one updown, menu untouched.
    gm::FrontendMenu menu;
    menu.reset();
    const gm::Screen before_screen = menu.screen();
    const std::size_t before_selected = menu.selected();
    const gm::UiSoundPlan down = gm::ui_sounds_for(menu, key(gm::KeyEvent::Down));
    CHECK(down.size() == 1);
    CHECK(has(down, gm::UiSound::up_down));
    CHECK(menu.screen() == before_screen);
    CHECK(menu.selected() == before_selected);

    // Enter on a MAIN art item (item 0 -> SOUND OPTIONS): select + selectm from
    // the doc, plus the model menuon because the screen changes.
    gm::FrontendMenu main_menu;
    main_menu.reset();
    CHECK(main_menu.selected_id() == 0);
    const gm::UiSoundPlan enter_first =
        gm::ui_sounds_for(main_menu, key(gm::KeyEvent::Enter));
    CHECK(enter_first.size() == 3); // select + selectm + menuon (model)
    CHECK(has(enter_first, gm::UiSound::select));
    CHECK(has(enter_first, gm::UiSound::select_m));
    CHECK(has(enter_first, gm::UiSound::menu_on));
    if (enter_first.size() == 3) {
        CHECK(enter_first.events[0].table_index == 510); // select
        CHECK(enter_first.events[1].table_index == 511); // selectm
        CHECK(enter_first.events[2].table_index == 425); // menuon (model)
    }

    // Enter that changes the screen also adds the model menuon event; the
    // ordering is select, selectm, menuon.
    gm::FrontendMenu submenu;
    submenu.reset();
    for (int i = 0; i < 3; ++i) {
        submenu.feed(key(gm::KeyEvent::Down));
    }
    CHECK(submenu.selected_id() == 3);
    const gm::UiSoundPlan enter_sub =
        gm::ui_sounds_for(submenu, key(gm::KeyEvent::Enter));
    CHECK(enter_sub.size() == 3);
    CHECK(has(enter_sub, gm::UiSound::select));
    CHECK(has(enter_sub, gm::UiSound::select_m));
    CHECK(has(enter_sub, gm::UiSound::menu_on));
    if (enter_sub.size() == 3) {
        CHECK(enter_sub.events[2].sound == gm::UiSound::menu_on);
        CHECK(enter_sub.events[2].table_index == 425);
    }

    // A toggle row is not distinguished by the doc: `selectm` plays on every
    // Enter, so PLAY OPTIONS item 0 (a toggle) gives the same pair and no
    // screen change -> no menuon.
    gm::FrontendMenu toggle;
    toggle.reset();
    CHECK(toggle.enter(gm::Screen::PlayOptions));
    CHECK(toggle.selected_id() == 0);
    const gm::UiSoundPlan enter_toggle =
        gm::ui_sounds_for(toggle, key(gm::KeyEvent::Enter));
    CHECK(enter_toggle.size() == 2);
    CHECK(has(enter_toggle, gm::UiSound::select));
    CHECK(has(enter_toggle, gm::UiSound::select_m));

    // Left/Right overrides: `Switch Left Right` -> vleft only.
    gm::FrontendMenu sound;
    sound.reset();
    CHECK(sound.enter(gm::Screen::SoundOptions));
    for (int i = 0; i < 2; ++i) {
        sound.feed(key(gm::KeyEvent::Down));
    }
    CHECK(sound.selected_id() == 4);
    for (gm::KeyEvent::Kind kind : {gm::KeyEvent::Left, gm::KeyEvent::Right}) {
        const gm::UiSoundPlan plan = gm::ui_sounds_for(sound, key(kind));
        CHECK(plan.size() == 1);
        CHECK(has(plan, gm::UiSound::v_left));
        CHECK(!has(plan, gm::UiSound::select));
        if (plan.size() == 1) {
            CHECK(plan.events[0].table_index == 804);
            CHECK(plan.events[0].name == "vleft");
        }
    }
    // The volume rows (0/2) are silent in the plan unless the host reports the
    // value changed: the automaton does not own volume values (model limit).
    gm::FrontendMenu volume;
    volume.reset();
    CHECK(volume.enter(gm::Screen::SoundOptions));
    CHECK(volume.selected_id() == 0);
    CHECK(gm::ui_sounds_for(volume, key(gm::KeyEvent::Left)).empty());
    const gm::UiSoundPlan volume_moved =
        gm::ui_sounds_for(volume, key(gm::KeyEvent::Left), true);
    CHECK(volume_moved.size() == 1);
    CHECK(has(volume_moved, gm::UiSound::up_down));
    // At the limit the host reports moved=false: silence.
    CHECK(gm::ui_sounds_for(volume, key(gm::KeyEvent::Right), false).empty());

    // GRAPHICS OPTIONS `Border` -> updown; plain list -> select.
    gm::FrontendMenu graphics;
    graphics.reset();
    CHECK(graphics.enter(gm::Screen::GraphicsOptions));
    graphics.feed(key(gm::KeyEvent::Down));
    CHECK(graphics.selected_id() == 1);
    const gm::UiSoundPlan border =
        gm::ui_sounds_for(graphics, key(gm::KeyEvent::Right));
    CHECK(border.size() == 1 && has(border, gm::UiSound::up_down));

    // Esc on any screen: no events at all.
    for (gm::Screen screen : gm::FrontendMenu::screens()) {
        gm::FrontendMenu probe;
        probe.reset();
        CHECK(probe.enter(screen));
        CHECK(gm::ui_sounds_for(probe, key(gm::KeyEvent::Escape)).empty());
    }

    // The terminal / not-yet-started states are silent and safe.
    gm::FrontendMenu fresh;
    CHECK(gm::ui_sounds_for(fresh, key(gm::KeyEvent::Down)).empty());
    CHECK(gm::ui_sounds_for(fresh, key(gm::KeyEvent::Enter)).empty());
    gm::FrontendMenu exited;
    exited.reset();
    CHECK(exited.enter(gm::Screen::Exit));
    CHECK(gm::ui_sounds_for(exited, key(gm::KeyEvent::Enter)).empty());

    // Boundaries: every screen x every key yields a finite plan, `vback` is
    // never emitted (the doc proves the handle has no reader).
    int plans = 0;
    for (gm::Screen screen : gm::FrontendMenu::screens()) {
        gm::FrontendMenu probe;
        probe.reset();
        CHECK(probe.enter(screen));
        for (gm::KeyEvent::Kind kind :
             {gm::KeyEvent::Up, gm::KeyEvent::Down, gm::KeyEvent::Left,
              gm::KeyEvent::Right, gm::KeyEvent::Enter, gm::KeyEvent::Escape,
              gm::KeyEvent::Backspace, gm::KeyEvent::Text, gm::KeyEvent::Other}) {
            const gm::UiSoundPlan plan = gm::ui_sounds_for(probe, key(kind));
            ++plans;
            CHECK(!has(plan, gm::UiSound::back));
            CHECK(plan.size() <= 3);
            for (const gm::UiSoundEvent& event : plan.events) {
                CHECK(event.table_index > 0);
                CHECK(!event.name.empty());
            }
        }
    }
    std::printf("ui_sounds: %d plan(s) over %d screens, one key at a time\n", plans,
                static_cast<int>(gm::FrontendMenu::screens().size()));
}

/// E: negatives on the level table.
void test_table_negatives() {
    gm::SoundSampleTable empty;
    CHECK(!gm::table_has(empty, 581));
    CHECK(!gm::table_has(empty, 510));
    CHECK(gm::table_slot(empty, "updown") == -1);

    gm::SoundSampleTable one;
    one.counts[7] = 3;
    one.names[7] = "UpDown"; // case-insensitive, like the 8-byte uppercase key
    CHECK(gm::table_slot(one, "updown") == 7);
    CHECK(gm::table_slot(one, "UPDOWN") == 7);
    CHECK(gm::table_slot(one, "select") == -1);
    CHECK(gm::table_has(one, 581));  // its name is present with count > 0
    CHECK(!gm::table_has(one, 510)); // `select` is missing
    CHECK(!gm::table_has(one, 636));

    // count == 0 is a miss even when the name is present.
    gm::SoundSampleTable zero;
    zero.names[0] = "updown";
    CHECK(gm::table_slot(zero, "updown") == -1);
    CHECK(!gm::table_has(zero, 581));

    // Unknown / out-of-range indices and empty names never crash.
    CHECK(!gm::table_has(zero, -1));
    CHECK(!gm::table_has(zero, 0));
    CHECK(!gm::table_has(zero, 9999));
    CHECK(gm::table_slot(zero, "") == -1);
}

} // namespace

int main(int argc, char** argv) {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    std::string level = "level0";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--level" && i + 1 < argc) {
            level = argv[++i];
        } else if (arg == "--game" && i + 1 < argc) {
            game = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--level NAME] [--game DIR]\n", argv[0]);
            return 2;
        }
    }

    test_samples();
    test_triggers();
    test_plan();
    test_table_negatives();

    std::error_code ec;
    if (!fs::is_regular_file(game / "sounds" / "SOU.IDX", ec)) {
        std::printf("ui_sounds: game not installed (%s), %d checks on the model only\n",
                    game.string().c_str(), g_checks);
        return g_failures == 0 ? 0 : 1;
    }

    const auto path = gmtest::resolve_game_file(game, level + ".sbt");
    CHECK(path.has_value());
    if (!path) {
        std::printf("ui_sounds: %s.sbt not found\n", level.c_str());
        return 1;
    }
    std::string error;
    const auto parsed = gm::parse_sbt(read_file(*path), error);
    CHECK(parsed.has_value());
    CHECK(parsed && parsed->sound_samples.has_value());
    if (!parsed || !parsed->sound_samples) {
        std::printf("ui_sounds: %s has no SOUND_SAMPLE_TABLE: %s\n", level.c_str(),
                    error.c_str());
        return 1;
    }

    const std::vector<RealRow> rows = check_real(game, *parsed->sound_samples);
    std::printf("ui_sounds: level %s, %d checks, %zu sample rows\n", level.c_str(),
                g_checks, rows.size());
    for (const RealRow& row : rows) {
        std::printf("  %-8s idx %3d  slot %2d  count %u  archive %-8s rec %llu B\n",
                    row.name.c_str(), row.index, row.slot, row.count,
                    row.archive_name.c_str(),
                    static_cast<unsigned long long>(row.archive_size));    }
    gm::FrontendMenu report;
    report.reset();
    const std::string enter_plan =
        plan_names(gm::ui_sounds_for(report, key(gm::KeyEvent::Enter)));
    const std::string down_plan =
        plan_names(gm::ui_sounds_for(report, key(gm::KeyEvent::Down)));
    std::printf("ui_sounds: enter plan [%s], down plan [%s]\n", enter_plan.c_str(),
                down_plan.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "ui_sounds: %d failure(s) in %d checks\n", g_failures,
                     g_checks);
        return 1;
    }
    std::printf("ui_sounds: ok\n");
    return 0;
}
