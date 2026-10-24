// Save menu screen model (docs/SAVE_MENU.md, docs/UI_SCREENS.md §5,
// docs/SAVE_SLOTS.md).
//
// No installation is required for the synthetic half: the saves are built with
// the engine's own writer (`gm::Save::to_bytes`, docs/SAVE_WRITER.md) into a
// temporary `SAVEGAME` directory.  The checks pin:
//
//   * 20 slot views with name/level/seconds from the header (`name[0x50]`,
//     `level[0x50]` and F1 / 91) and the `::: empty slot :::` row label;
//   * the 6-row window: `visible_first` 0 for selections 0/5, 1 for 6 and 13
//     after walking to the last reachable slot 18 - slot 19 is never
//     selectable and `move()` never lands on it;
//   * load mode: occupied slots are selectable and yield `load`, empty and
//     corrupt slots are refused with `event() == none` and a text;
//   * save mode: empty, occupied and corrupt slots all yield `save` (the
//     original has no overwrite confirmation, UI_SCREENS §5.2), a missing
//     `SAVEGAME` directory is refused with a text;
//   * Esc -> `back`, the `move(-1)`/`move(+1)` clamps at both edges, two-run
//     determinism, and the real shipped saves when the install is present.
//
// The numbers printed at the end are quoted in docs/SAVE_MENU.md §5.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "res/save.h"
#include "ui/save_menu.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string in_quotes(std::string_view value) { return "\"" + std::string(value) + "\""; }

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

/// A minimal valid container: a 0x13c header (title, level, F1 clock, F2
/// mission) and the `0xff` terminator.
std::vector<std::uint8_t> make_save(const std::string& title, const std::string& level,
                                    std::uint32_t mission, std::uint32_t clock) {
    gm::Save save;
    save.header.title = title;
    save.header.level_name = level;
    save.header.fields[1] = clock;   // F1, the level clock (docs/SAVE_FIELDS.md §2)
    save.header.fields[2] = mission; // F2, next mission (proven)
    std::string error;
    const auto bytes = save.to_bytes(error);
    if (!bytes) {
        check(false, "writer refused a synthetic save: " + error);
        return {};
    }
    return *bytes;
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        check(false, "cannot create " + path.string());
        return;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// Fresh `<temp>/<name>/SAVEGAME` (no files).
fs::path make_empty_game(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SAVEGAME", ec);
    return root;
}

/// Fresh `<temp>/<name>/SAVEGAME` with `count` valid saves (slot N: name
/// `SLOT N`, level `LEVEL N`, mission N + 1, clock 91 * N + 45).
fs::path make_game(const std::string& name, int count) {
    const fs::path root = make_empty_game(name);
    for (int i = 0; i < count; ++i) {
        write_file(root / "SAVEGAME" / ("saved" + std::to_string(i) + ".sbt"),
                   make_save("SLOT " + std::to_string(i), "LEVEL" + std::to_string(i),
                             static_cast<std::uint32_t>(i + 1),
                             static_cast<std::uint32_t>(91 * i + 45)));
    }
    return root;
}

const gm::SaveSlotView& slot(const gm::SaveMenu& menu, int index) { return menu.at(index); }

// --------------------------------------------------------------------- cases

void test_before_open() {
    gm::SaveMenu menu;
    check(menu.slots().size() == 20, "20 empty views before open");
    check(!menu.is_open(), "not open before open()");
    check(menu.selected() == 0 && menu.visible_first() == 0, "cursor 0/0 before open");
    check(menu.event() == gm::SaveMenuEvent::none, "no event before open");
    for (int i = 0; i < gm::SaveMenu::kSlots; ++i) {
        check(!slot(menu, i).occupied && !slot(menu, i).selectable && slot(menu, i).index == i,
              "view " + std::to_string(i) + " is empty before open");
    }
    menu.move(3);
    check(menu.selected() == 0, "move() before open is a no-op");
    std::string error;
    check(!menu.activate(&error) && menu.event() == gm::SaveMenuEvent::none,
          "activate() before open sets no event");
    check(contains(error, "not open"), "activate() before open explains itself: " + in_quotes(error));
    check(menu.label(0) == gm::SaveMenu::kEmptyLabel, "an empty row is ::: empty slot :::");
    check(menu.label(-1).empty() && menu.label(6).empty(), "out-of-range rows are empty");
}

void test_window() {
    const fs::path game = make_game("gm_save_menu_window", 20);
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::load, game);
    check(menu.is_open() && menu.mode() == gm::SaveMenuMode::load, "open(load)");
    check(menu.error().empty(), "20 readable saves -> no error: " + in_quotes(menu.error()));
    check(menu.slots().size() == 20, "20 slot views");
    check(menu.visible_rows() == 6, "6 visible rows");
    check(menu.selected() == 0 && menu.visible_first() == 0, "open starts at slot 0, window 0");
    check(slot(menu, 19).occupied && !slot(menu, 19).selectable,
          "slot 19 is occupied but never selectable");

    menu.move(5);
    const int first_at_5 = menu.visible_first();
    check(menu.selected() == 5 && first_at_5 == 0, "move(5) -> slot 5, window still 0");
    menu.move(1);
    const int first_at_6 = menu.visible_first();
    check(menu.selected() == 6 && first_at_6 == 1, "move(1) -> slot 6, window 1");

    menu.move(100);
    check(menu.selected() == gm::SaveMenu::kLastReachable && menu.selected() == 18,
          "a long move lands on the last reachable slot 18, not 19");
    check(menu.visible_first() == gm::SaveMenu::kMaxFirst && menu.visible_first() == 13,
          "window at 13 shows slots 13..18");
    check(menu.label(0) == "SLOT 13" && menu.label(5) == "SLOT 18",
          "rows at window 13 are SLOT 13..SLOT 18");
    for (int row = 1; row < 6; ++row) {
        check(menu.label(row) == "SLOT " + std::to_string(13 + row),
              "row " + std::to_string(row) + " of window 13");
    }

    menu.move(1);
    check(menu.selected() == 18 && menu.visible_first() == 13,
          "move(+1) at 18 is clamped (slot 19 unreachable)");
    menu.move(-1);
    check(menu.selected() == 17 && menu.visible_first() == 13,
          "move(-1) from 18 moves the highlight inside the same window");
    menu.move(-5);
    check(menu.selected() == 12 && menu.visible_first() == 12,
          "crossing the top edge scrolls the window to 12");

    menu.move(-100);
    check(menu.selected() == 0 && menu.visible_first() == 0, "a long move back lands on slot 0");
    menu.move(-1);
    check(menu.selected() == 0 && menu.visible_first() == 0, "move(-1) at 0 is clamped");
    std::printf("save_menu: window 20 slots, visible=6, first(0,5,6,19)=(0,0,1,13), "
                "slot19 selectable=0\n");
}

void test_load_mode() {
    // Occupied: 0, 2, 5 - the gaps must be skipped.
    const fs::path game = make_empty_game("gm_save_menu_load");
    for (int i : {0, 2, 5}) {
        write_file(game / "SAVEGAME" / ("saved" + std::to_string(i) + ".sbt"),
                   make_save("SAVE " + std::to_string(i), "LEVEL0",
                             static_cast<std::uint32_t>(i + 1), 91));
    }
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::load, game);
    check(menu.error().empty(), "three readable saves -> no error: " + in_quotes(menu.error()));
    check(menu.selected() == 0, "the initial selection is the first occupied slot");
    check(slot(menu, 1).occupied == false && !slot(menu, 1).selectable,
          "an empty slot is not selectable in load mode");
    menu.move(1);
    check(menu.selected() == 2, "move(+1) skips empty slot 1 -> 2");
    menu.move(1);
    check(menu.selected() == 5, "move(+1) skips empty slots 3/4 -> 5");
    menu.move(1);
    check(menu.selected() == 5, "move(+1) past the last occupied slot is clamped");
    menu.move(-1);
    check(menu.selected() == 2, "move(-1) skips back to slot 2");

    std::string error = "sentinel";
    check(menu.activate(&error) && menu.event() == gm::SaveMenuEvent::load,
          "activate() on an occupied slot yields load");
    check(error.empty(), "a successful load clears the error: " + in_quotes(error));

    // Empty directory: nothing selectable, load refused with a text.
    gm::SaveMenu empty;
    empty.open(gm::SaveMenuMode::load, make_empty_game("gm_save_menu_empty"));
    check(empty.error().empty(), "an empty SAVEGAME directory is not a listing error");
    check(empty.selected() == 0 && empty.visible_first() == 0, "empty directory starts at 0/0");
    check(!slot(empty, 0).selectable && !slot(empty, 0).occupied, "nothing is selectable");
    error = "sentinel";
    check(!empty.activate(&error) && empty.event() == gm::SaveMenuEvent::none,
          "load of an empty slot produces no event");
    check(error == "save slot 0 is empty", "load of an empty slot explains itself: " +
                                               in_quotes(error));

    // Only the last reachable slot is occupied: the window opens on 13..18.
    const fs::path tail = make_empty_game("gm_save_menu_tail");
    write_file(tail / "SAVEGAME" / "saved18.sbt", make_save("TAIL", "LEVEL0", 1, 91));
    gm::SaveMenu last;
    last.open(gm::SaveMenuMode::load, tail);
    check(last.selected() == 18 && last.visible_first() == 13,
          "the only occupied slot 18 opens with window 13");
    check(last.label(5) == "TAIL" && last.label(0) == gm::SaveMenu::kEmptyLabel,
          "row 5 of the window is the save, the earlier rows are empty");
    last.move(-1);
    check(last.selected() == 18 && last.visible_first() == 13,
          "no empty slot to skip up to: the cursor stays on 18");
    std::printf("save_menu: load occupied -> load (0,2,5, gaps skipped); "
                "empty -> none \"save slot 0 is empty\"\n");
}

void test_save_mode() {
    // Empty slot in a fresh directory.
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, make_empty_game("gm_save_menu_save_empty"));
    check(menu.is_open() && menu.mode() == gm::SaveMenuMode::save, "open(save)");
    check(slot(menu, 0).selectable && !slot(menu, 0).occupied,
          "an empty slot is selectable in save mode");
    std::string error = "sentinel";
    check(menu.activate(&error) && menu.event() == gm::SaveMenuEvent::save,
          "save into an empty slot yields save");
    check(error.empty(), "a successful save clears the error");

    // Occupied slot: a rewrite, and the original does not confirm it.
    const fs::path occupied = make_game("gm_save_menu_save_occupied", 1);
    gm::SaveMenu rewrite;
    rewrite.open(gm::SaveMenuMode::save, occupied);
    check(slot(rewrite, 0).occupied && slot(rewrite, 0).selectable,
          "an occupied slot stays selectable in save mode");
    error = "sentinel";
    check(rewrite.activate(&error) && rewrite.event() == gm::SaveMenuEvent::save,
          "save over an occupied slot yields save (no overwrite prompt, UI_SCREENS §5.2)");
    check(error.empty(), "the overwrite path carries no error text");
    check(rewrite.at(0).name == "SLOT 0", "the occupied slot keeps its name for the caller");

    // A corrupt slot can still be overwritten.
    const fs::path corrupt = make_empty_game("gm_save_menu_save_corrupt");
    const std::string junk = "not a save";
    write_file(corrupt / "SAVEGAME" / "saved0.sbt",
               std::vector<std::uint8_t>(junk.begin(), junk.end()));
    gm::SaveMenu over_bad;
    over_bad.open(gm::SaveMenuMode::save, corrupt);
    check(!over_bad.at(0).occupied && over_bad.at(0).selectable,
          "a corrupt slot is empty but selectable in save mode");
    check(!over_bad.error().empty(), "the corrupt file is still reported in error()");
    error = "sentinel";
    check(over_bad.activate(&error) && over_bad.event() == gm::SaveMenuEvent::save,
          "save over a corrupt slot yields save");
    check(error.empty(), "overwriting a corrupt slot carries no error text");

    rewrite.move(100);
    check(rewrite.selected() == 18 && rewrite.visible_first() == 13,
          "save mode also stops at slot 18");

    // Missing SAVEGAME directory: nothing to list, save refused with a text.
    const fs::path missing = fs::temp_directory_path() / "gm_save_menu_no_such_game";
    std::error_code ec;
    fs::remove_all(missing, ec);
    gm::SaveMenu no_dir;
    no_dir.open(gm::SaveMenuMode::save, missing);
    check(!no_dir.has_save_directory(), "missing SAVEGAME is detected");
    check(no_dir.error().empty(), "a missing directory is not a listing error");
    check(!no_dir.at(0).selectable && !no_dir.at(0).occupied, "no slot is selectable");
    error = "sentinel";
    check(!no_dir.activate(&error) && no_dir.event() == gm::SaveMenuEvent::none,
          "save without a directory produces no event");
    check(contains(error, "SAVEGAME") && contains(error, "cannot save"),
          "save without a directory explains itself: " + in_quotes(error));

    gm::SaveMenu no_dir_load;
    no_dir_load.open(gm::SaveMenuMode::load, missing);
    error = "sentinel";
    check(!no_dir_load.activate(&error) && no_dir_load.event() == gm::SaveMenuEvent::none,
          "load without a directory produces no event");
    check(error == "save slot 0 is empty", "load without a directory: " + in_quotes(error));
    std::printf("save_menu: save empty/occupied/corrupt -> save (no overwrite prompt), "
                "missing dir refused\n");
}

void test_header_fields() {
    const fs::path game = make_empty_game("gm_save_menu_fields");
    // 91 * 123 + 45 ticks -> 123 whole seconds (floor, like F1 / 91).
    write_file(game / "SAVEGAME" / "saved0.sbt",
               make_save("LONG RUN", "LEVEL3", 7, 91 * 123 + 45));
    write_file(game / "SAVEGAME" / "saved1.sbt", make_save("SECOND", "LEVEL0", 1, 0));

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::load, game);
    check(menu.error().empty(), "synthetic saves read without error: " + in_quotes(menu.error()));
    check(menu.at(0).name == "LONG RUN", "name from the header: " + in_quotes(menu.at(0).name));
    check(menu.at(0).level == "LEVEL3", "level from the header: " + in_quotes(menu.at(0).level));
    check(menu.at(0).seconds == 123,
          "seconds = F1 / 91 = 123: " + std::to_string(menu.at(0).seconds));
    check(menu.at(0).occupied && menu.at(0).selectable, "the save is occupied and selectable");
    check(menu.at(1).occupied && menu.at(1).name == "SECOND",
          "a second save keeps its name: " + in_quotes(menu.at(1).name));
    check(menu.at(1).seconds == 0, "F1 = 0 -> 0 seconds");
    check(menu.at(2).seconds == 0 && menu.at(2).level.empty(),
          "an empty slot has no level and 0 seconds");
    std::printf("save_menu: header -> name=\"LONG RUN\" level=LEVEL3 F1=11238 seconds=123\n");
}

void test_negatives() {
    // saved0 is junk, saved1 is fine: the corrupt slot is skipped, slot 1 loads.
    const fs::path game = make_game("gm_save_menu_corrupt", 2);
    const std::string junk = "not a save";
    write_file(game / "SAVEGAME" / "saved0.sbt",
               std::vector<std::uint8_t>(junk.begin(), junk.end()));
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::load, game);
    check(!menu.error().empty(), "a corrupt save is reported in error()");
    check(!menu.at(0).occupied && !menu.at(0).selectable,
          "the corrupt slot is empty and not selectable");
    check(menu.at(1).occupied && menu.at(1).selectable, "the next slot still loads");
    check(menu.selected() == 1, "the selection starts on the first readable slot");
    std::string error = "sentinel";
    check(menu.activate(&error) && menu.event() == gm::SaveMenuEvent::load,
          "the readable slot loads past the corrupt one");
    check(error.empty(), "no error text on a successful load after the bad slot");

    // Only a corrupt file: load is refused with the parse problem, no crash.
    const fs::path only_bad = make_empty_game("gm_save_menu_only_bad");
    write_file(only_bad / "SAVEGAME" / "saved0.sbt",
               std::vector<std::uint8_t>(junk.begin(), junk.end()));
    gm::SaveMenu bad;
    bad.open(gm::SaveMenuMode::load, only_bad);
    check(!bad.error().empty() && contains(bad.error(), "saved0.sbt"),
          "the corrupt file is named in error(): " + in_quotes(bad.error()));
    check(!bad.at(0).occupied && !bad.at(0).selectable, "a corrupt slot is not selectable");
    error = "sentinel";
    check(!bad.activate(&error) && bad.event() == gm::SaveMenuEvent::none,
          "load of a corrupt slot produces no event");
    check(contains(error, "cannot be loaded") && contains(error, "save slot 0"),
          "load of a corrupt slot explains itself: " + in_quotes(error));

    // A truncated (header-only, no 0xff) file is corrupt too.
    write_file(only_bad / "SAVEGAME" / "saved0.sbt", std::vector<std::uint8_t>(0x13c, 0));
    bad.open(gm::SaveMenuMode::load, only_bad);
    check(!bad.error().empty(), "a truncated save is reported");
    error.clear();
    check(!bad.activate(&error) && bad.event() == gm::SaveMenuEvent::none,
          "a truncated save cannot be loaded");
    check(contains(error, "cannot be loaded"), "the truncated-save text: " + in_quotes(error));

    // Esc (both screens carry ESC TO CANCEL, UI_SCREENS §3.6).
    bad.escape();
    check(bad.event() == gm::SaveMenuEvent::back, "escape() yields back");
    check(bad.activate(&error) == false && bad.event() == gm::SaveMenuEvent::none,
          "activate() after back() starts from none again");
    bad.escape();
    bad.clear_event();
    check(bad.event() == gm::SaveMenuEvent::none, "clear_event() resets the event");
    std::printf("save_menu: corrupt -> error text + none \"save slot 0 cannot be loaded: ...\"; "
                "esc -> back\n");
}

void test_determinism() {
    const fs::path game = make_game("gm_save_menu_determinism", 8);
    gm::SaveMenu a;
    gm::SaveMenu b;
    a.open(gm::SaveMenuMode::load, game);
    b.open(gm::SaveMenuMode::load, game);
    a.move(3);
    b.move(3);
    bool same = a.selected() == b.selected() && a.visible_first() == b.visible_first() &&
                a.slots().size() == b.slots().size() && a.event() == b.event();
    for (int i = 0; i < gm::SaveMenu::kSlots; ++i) {
        same = same && a.at(i).index == b.at(i).index && a.at(i).occupied == b.at(i).occupied &&
               a.at(i).selectable == b.at(i).selectable && a.at(i).name == b.at(i).name &&
               a.at(i).level == b.at(i).level && a.at(i).seconds == b.at(i).seconds;
    }
    check(same, "two opens of the same directory agree after the same moves");

    // Re-opening the same object resets the cursor and the event.
    a.move(2);
    a.escape();
    a.open(gm::SaveMenuMode::load, game);
    check(a.selected() == 0 && a.visible_first() == 0, "re-open resets the cursor/window");
    check(a.event() == gm::SaveMenuEvent::none, "re-open clears the event");
    std::printf("save_menu: deterministic (same dir + same moves -> same views/cursor)\n");
}

void test_installed_saves() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const fs::path dir = game / "SAVEGAME";
    if (!fs::exists(dir / "saved0.sbt") || !fs::exists(dir / "saved1.sbt")) {
        std::printf("save_menu: shipped saves not installed, skipping the real check\n");
        return;
    }
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::load, game);
    check(menu.error().empty(), "the shipped SAVEGAME reads without error: " + in_quotes(menu.error()));
    check(menu.slots().size() == 20, "the shipped list has 20 slots");
    check(menu.at(0).occupied, "saved0 is occupied");
    check(menu.at(0).name == "A SAVED GAME", "saved0 title: " + in_quotes(menu.at(0).name));
    check(menu.at(0).level == "LEVEL0", "saved0 level: " + in_quotes(menu.at(0).level));
    check(menu.at(0).seconds == 47, "saved0 seconds: " + std::to_string(menu.at(0).seconds));
    check(menu.at(1).occupied && menu.at(1).name == "2",
          "saved1 title: " + in_quotes(menu.at(1).name));
    check(menu.at(1).level == "LEVEL0", "saved1 level: " + in_quotes(menu.at(1).level));
    check(menu.at(1).seconds == 89, "saved1 seconds: " + std::to_string(menu.at(1).seconds));
    check(menu.at(2).occupied == false, "slot 2 of the shipped install is empty");
    check(menu.label(0) == "A SAVED GAME" && menu.label(1) == "2" &&
              menu.label(2) == gm::SaveMenu::kEmptyLabel,
          "shipped rows 0/1/2 are title, title, placeholder");
    check(menu.selected() == 0, "the shipped menu starts on slot 0");

    std::string error;
    check(menu.activate(&error) && menu.event() == gm::SaveMenuEvent::load,
          "the shipped saved0 yields load");
    check(error.empty(), "loading the shipped saved0 has no error text");

    // The same install in save mode: slot 0 is an overwrite, and the doc says
    // there is no prompt.
    gm::SaveMenu save;
    save.open(gm::SaveMenuMode::save, game);
    check(save.at(0).occupied && save.at(0).selectable,
          "the shipped saved0 is selectable for overwrite");
    error = "sentinel";
    check(save.activate(&error) && save.event() == gm::SaveMenuEvent::save,
          "the shipped saved0 yields save on Enter");
    check(error.empty(), "no overwrite confirmation text");
    std::printf("save_menu: shipped saved0 \"A SAVED GAME\" level=LEVEL0 seconds=47; "
                "saved1 \"2\" level=LEVEL0 seconds=89\n");
}

} // namespace

int main() {
    test_before_open();
    test_window();
    test_load_mode();
    test_save_mode();
    test_header_fields();
    test_negatives();
    test_determinism();
    test_installed_saves();

    if (failures == 0) {
        std::printf("save_menu: %d checks, ok (20 slots, %d visible, slot 19 unreachable)\n",
                    checks, gm::SaveMenu::kVisible);
    }
    return failures == 0 ? 0 : 1;
}
