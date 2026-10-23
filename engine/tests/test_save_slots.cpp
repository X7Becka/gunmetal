// Save slot list (docs/SAVE_SLOTS.md, docs/UI_SCREENS.md §5.1).
//
// No installation is required: the saves are generated with the engine's own
// writer (`gm::Save::to_bytes`, docs/SAVE_WRITER.md) into a temporary
// `SAVEGAME` directory.  The checks pin:
//
//   * 20 table entries with the `::: empty slot :::` label (`0x4bc3b0`) and an
//     empty directory that is not an error;
//   * the name/level/mission read from the header (`name[0x50]`, `level[0x50]`,
//     F2 at +0xac);
//   * the 0..13 scroll (`scroll + 6 < 20`), its boundaries and the six visible
//     labels, including the unreachable slot 19;
//   * the selection moving through the window edge and scrolling with it;
//   * the overwrite confirmation model (occupied -> needs_confirm, cancel
//     restores the selection, navigation is modal while pending);
//   * corrupt files (first error reported, other slots still loaded), two-run
//     determinism and the real shipped saves when the install is present.
//
// The numbers printed at the end are quoted in docs/SAVE_SLOTS.md §6.

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
#include "ui/save_slots.h"

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

/// A minimal valid container: a 0x13c header (title, level, F2) and `0xff`.
std::vector<std::uint8_t> make_save(const std::string& title, const std::string& level,
                                    std::uint32_t mission) {
    gm::Save save;
    save.header.title = title;
    save.header.level_name = level;
    save.header.fields[2] = mission; // F2, "next mission/level number" (proven)
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

/// Fresh `<temp>/<name>/SAVEGAME` with `count` valid saves (slot N: name
/// `SLOT N`, level `LEVEL N`, mission N + 1).
fs::path make_game(const std::string& name, int count) {
    const fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SAVEGAME", ec);
    for (int i = 0; i < count; ++i) {
        const std::string title = "SLOT " + std::to_string(i);
        write_file(root / "SAVEGAME" / ("saved" + std::to_string(i) + ".sbt"),
                   make_save(title, "LEVEL" + std::to_string(i),
                             static_cast<std::uint32_t>(i + 1)));
    }
    return root;
}

std::size_t placeholder_rows(const gm::SaveSlotList& list) {
    std::size_t count = 0;
    for (int row = 0; row < list.visible_rows(); ++row) {
        if (list.item_label(row) == gm::SaveSlotList::kEmptyLabel) {
            ++count;
        }
    }
    return count;
}

// --------------------------------------------------------------------- cases

void test_empty() {
    // Before refresh: 20 empty slots, empty cursor, no pending overwrite.
    gm::SaveSlotList fresh;
    check(fresh.count() == 20, "count() is 20 before refresh");
    check(fresh.present_count() == 0, "no slot is present before refresh");
    check(fresh.scroll() == 0 && fresh.selected() == 0, "initial scroll/selection are 0");
    check(!fresh.overwrite_pending(), "no overwrite pending initially");
    check(!fresh.select(-1) && !fresh.select(20), "out-of-range select is refused");
    check(fresh.scroll() == 0 && fresh.selected() == 0, "refused select changes nothing");

    // An existing but empty SAVEGAME directory.
    const fs::path game = make_game("gm_save_slots_empty", 0);
    std::string error = "sentinel";
    gm::SaveSlotList list;
    list.refresh(game, &error);
    check(error.empty(), "empty SAVEGAME directory is not an error: " + in_quotes(error));
    check(list.count() == 20 && list.present_count() == 0, "empty directory -> 20 empty slots");
    check(placeholder_rows(list) == 6, "all six rows are ::: empty slot :::");
    check(list.visible_rows() == 6 && list.scroll() == 0 && list.selected() == 0,
          "empty directory starts at row 0 of the window");
    check(!list.scroll_up(), "scroll_up at 0 is refused");
    int moved = 0;
    while (list.scroll_down()) {
        ++moved;
    }
    check(moved == 13 && list.scroll() == 13, "13 scroll steps reach the 0..13 bound");
    check(!list.scroll_down(), "the 14th scroll step is refused");
    std::printf("save_slots: empty dir -> slots=20 present=0 labels=6 scroll=0..13\n");

    // A missing directory is an empty list, not an error either.
    error = "sentinel";
    list.refresh(fs::temp_directory_path() / "gm_save_slots_no_such_game", &error);
    check(error.empty(), "missing save directory is not an error: " + in_quotes(error));
    check(list.present_count() == 0, "missing directory -> no present slot");

    // A null error pointer is allowed.
    list.refresh(fs::temp_directory_path() / "gm_save_slots_no_such_game", nullptr);
    check(list.present_count() == 0, "refresh(nullptr) is safe");
}

void test_one_and_header_fields() {
    const fs::path game = make_game("gm_save_slots_one", 1);
    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);
    check(error.empty(), "one save reads without error: " + in_quotes(error));
    check(list.present_count() == 1, "exactly one slot is present");
    const gm::SaveSlot& slot = list.at(0);
    check(slot.index == 0 && slot.present, "slot 0 is present");
    check(slot.name == "SLOT 0", "name comes from the header: " + in_quotes(slot.name));
    check(slot.level == 0, "level is the suffix of level[0x50]: " + std::to_string(slot.level));
    check(slot.mission == 1, "mission is F2 (+0xac): " + std::to_string(slot.mission));
    check(slot.detail == "LEVEL0, mission 1", "detail: " + in_quotes(slot.detail));
    check(!list.at(1).present && list.at(1).name.empty(), "slot 1 is an empty entry");
    check(!list.at(19).present && list.at(19).index == 19, "slot 19 exists as an empty entry");
    check(!list.at(99).present && list.at(99).index == 19, "at() clamps out-of-range indexes");
    check(list.item_label(0) == "SLOT 0", "row 0 shows the save name");
    check(list.item_label(1) == gm::SaveSlotList::kEmptyLabel, "row 1 shows ::: empty slot :::");
    check(list.item_label(-1).empty() && list.item_label(6).empty(),
          "out-of-range rows are empty strings");
    std::printf("save_slots: 1 save -> present=1 name=\"SLOT 0\" level=0 mission=1\n");

    // A non-LEVEL level name keeps level = -1 but is still readable.
    const fs::path odd = make_game("gm_save_slots_odd_level", 0);
    write_file(odd / "SAVEGAME" / "saved3.sbt", make_save("ODD", "TUTORIAL", 7));
    list.refresh(odd, &error);
    check(error.empty() && list.present_count() == 1, "the odd save loads");
    check(list.at(3).present && list.at(3).level == -1, "TUTORIAL -> level -1");
    check(list.at(3).mission == 7 && list.at(3).detail == "TUTORIAL, mission 7",
          "the raw level name stays in detail: " + in_quotes(list.at(3).detail));
}

void test_six_slots() {
    const fs::path game = make_game("gm_save_slots_six", 6);
    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);
    check(error.empty(), "six saves read without error");
    check(list.present_count() == 6, "six present slots");
    for (int row = 0; row < 6; ++row) {
        const std::string want = "SLOT " + std::to_string(row);
        check(list.item_label(row) == want, "row " + std::to_string(row) + " is " + want);
    }
    check(list.scroll() == 0 && list.selected() == 0, "six slots fit in the first window");
    std::printf("save_slots: 6 saves -> present=6 window=0..5 labels=6\n");
}

void test_scroll_bounds() {
    const fs::path game = make_game("gm_save_slots_twenty", 20);
    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);
    check(error.empty(), "twenty saves read without error: " + in_quotes(error));
    check(list.present_count() == 20, "twenty present slots");

    int moved = 0;
    while (list.scroll_down()) {
        ++moved;
    }
    check(moved == 13, "scroll stops after 13 steps: " + std::to_string(moved));
    check(list.scroll() == gm::SaveSlotList::kMaxScroll, "scroll == 13 (scroll + 6 < 20)");
    check(!list.scroll_down(), "scroll_down at 13 is refused");
    check(list.scroll() == 13, "the refused scroll changed nothing");

    check(list.item_label(0) == "SLOT 13", "window at 13 starts at slot 13");
    check(list.item_label(5) == "SLOT 18", "window at 13 ends at slot 18");
    check(list.item_label(6).empty(), "there is no seventh row");
    check(list.at(19).present, "slot 19 exists in the table");
    check(!list.select(19), "slot 19 is unreachable with scroll <= 13");
    check(list.scroll() == 13 && list.selected() >= 13, "the refused select(19) changed nothing");

    check(list.select(18), "slot 18 is selectable");
    check(list.selected() == 18 && list.scroll() == 13, "select(18) -> selected 18, scroll 13");
    check(!list.select_down(), "select_down at 18 (bottom of the 13..18 window) is refused");

    check(list.select(0), "select(0) is accepted");
    check(list.selected() == 0 && list.scroll() == 0, "select(0) scrolls the window back to 0");
    check(!list.scroll_up(), "scroll_up at 0 is refused");

    int down = 0;
    while (list.scroll_down()) {
        ++down;
    }
    check(down == 13 && list.scroll() == 13, "the window walks 0 -> 13");
    int up_steps = 0;
    while (list.scroll_up()) {
        ++up_steps;
    }
    check(up_steps == 13 && list.scroll() == 0, "scroll_up walks 13 -> 0");
    check(list.selected() >= 0 && list.selected() <= 5, "the selection stays inside the window");
    std::printf("save_slots: 20 saves -> present=20 scroll=0..13 window=13..18 (slot 19 unreachable)\n");
}

void test_selection_window() {
    const fs::path game = make_game("gm_save_slots_select", 20);
    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);

    int downs = 0;
    for (int i = 0; i < 5; ++i) {
        if (list.select_down()) {
            ++downs;
        }
    }
    check(downs == 5 && list.selected() == 5 && list.scroll() == 0,
          "rows 0..5 stay in the first window (selected 5, scroll 0)");
    check(list.select_down(), "the sixth row down is accepted");
    check(list.selected() == 6 && list.scroll() == 1,
          "crossing the bottom edge scrolls the window to 1");
    check(list.select_up(), "select_up is accepted");
    check(list.selected() == 5 && list.scroll() == 1, "the selection moves back inside the window");

    for (int i = 0; i < 4; ++i) {
        list.select_up();
    }
    check(list.selected() == 1 && list.scroll() == 1, "selection 1 is the top of window 1..6");
    check(list.select_up(), "one more up crosses the top edge");
    check(list.selected() == 0 && list.scroll() == 0, "crossing the top edge scrolls to 0");
    check(!list.select_up(), "select_up at slot 0 is refused");
    std::printf("save_slots: selection 0..5 -> scroll 0; row 6 -> scroll 1; back up -> scroll 0\n");
}

void test_overwrite() {
    const fs::path game = make_game("gm_save_slots_overwrite", 20);
    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);

    check(list.needs_confirm(0), "an occupied slot needs confirmation");
    check(list.select(3) && !list.overwrite_pending(), "no confirmation before a request");

    list.request_overwrite(3);
    check(list.overwrite_pending(), "request_overwrite enters the pending state");
    check(list.pending_slot() == 3 && list.selected() == 3,
          "the request moves the selection to the slot");
    check(!list.scroll_down() && !list.select_up() && !list.select(0),
          "navigation is modal while the confirmation is pending");
    check(list.scroll() == 0 && list.selected() == 3, "the refused navigation changed nothing");
    list.cancel();
    check(!list.overwrite_pending(), "cancel leaves the pending state");
    check(list.selected() == 3, "cancel keeps the selection when it did not move");

    // Cancel restores the selection captured at the request.
    check(list.select(0), "select 0");
    list.request_overwrite(7);
    check(list.selected() == 7 && list.pending_slot() == 7, "request moves selection 0 -> 7");
    list.cancel();
    check(!list.overwrite_pending() && list.selected() == 0,
          "cancel restores the selection the request found");

    // Confirmation keeps the selection on the slot.
    list.request_overwrite(9);
    list.confirm();
    check(!list.overwrite_pending() && list.selected() == 9,
          "confirm keeps the selection on the confirmed slot");
    list.confirm();
    check(!list.overwrite_pending(), "confirm without a request is a no-op");
    list.cancel();
    check(!list.overwrite_pending(), "cancel without a request is a no-op");

    // Empty slots are refused: no dialog for a slot that needs no overwrite.
    gm::SaveSlotList gap;
    gap.refresh(make_game("gm_save_slots_gap", 0), &error);
    check(!gap.needs_confirm(4), "an empty slot needs no confirmation");
    gap.select(2);
    gap.request_overwrite(4);
    check(!gap.overwrite_pending() && gap.selected() == 2,
          "request_overwrite on an empty slot is refused");
    check(!gap.needs_confirm(-1) && !gap.needs_confirm(20),
          "needs_confirm is false outside 0..19");

    // A second request keeps the first restore point.
    list.select(1);
    list.request_overwrite(5);
    list.request_overwrite(11);
    check(list.pending_slot() == 11 && list.selected() == 11, "a second request moves the slot");
    list.cancel();
    check(list.selected() == 1, "cancel returns to the pre-request selection (1)");
    std::printf("save_slots: overwrite needs_confirm=1, cancel restores selection, confirm keeps it\n");
}

void test_corrupt_and_determinism() {
    const fs::path game = make_game("gm_save_slots_corrupt", 2);
    const std::string junk = "not a save";
    write_file(game / "SAVEGAME" / "saved0.sbt",
               std::vector<std::uint8_t>(junk.begin(), junk.end()));

    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);
    check(!error.empty(), "a corrupt save is reported in *error");
    check(list.present_count() == 1 && list.at(0).present == false,
          "the corrupt slot stays empty, the other slot still loads");
    check(list.at(1).present && list.at(1).name == "SLOT 1", "slot 1 survived the bad slot 0");

    // A truncated (header-only) file is 0x13c bytes without the 0xff terminator.
    write_file(game / "SAVEGAME" / "saved0.sbt", std::vector<std::uint8_t>(0x13c, 0));
    error.clear();
    list.refresh(game, &error);
    check(!error.empty(), "a truncated save is reported");

    // Determinism: the same directory gives the same labels twice.
    const fs::path clean = make_game("gm_save_slots_determinism", 20);
    gm::SaveSlotList a;
    gm::SaveSlotList b;
    a.refresh(clean, &error);
    b.refresh(clean, &error);
    bool same = a.present_count() == b.present_count() && a.scroll() == b.scroll();
    for (int i = 0; i < gm::SaveSlotList::kSlots; ++i) {
        same = same && a.at(i).name == b.at(i).name && a.at(i).present == b.at(i).present &&
               a.at(i).level == b.at(i).level && a.at(i).mission == b.at(i).mission;
    }
    check(same, "two refreshes of the same directory agree");
    std::printf("save_slots: corrupt slot reported (error set), slot 1 still loaded, two runs equal\n");

    // Lower-case `savegame` is the spelling the original opens (`0x4ab460`).
    const fs::path lower = fs::temp_directory_path() / "gm_save_slots_lower";
    std::error_code ec;
    fs::remove_all(lower, ec);
    fs::create_directories(lower / "savegame", ec);
    write_file(lower / "savegame" / "saved2.sbt", make_save("LOWER", "LEVEL2", 3));
    list.refresh(lower, &error);
    check(error.empty() && list.present_count() == 1 && list.at(2).name == "LOWER",
          "lower-case savegame directory is found too");
}

void test_installed_saves() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const fs::path dir = game / "SAVEGAME";
    if (!fs::exists(dir / "saved0.sbt") || !fs::exists(dir / "saved1.sbt")) {
        std::printf("save_slots: shipped saves not installed, skipping the real check\n");
        return;
    }
    std::string error;
    gm::SaveSlotList list;
    list.refresh(game, &error);
    check(error.empty(), "the shipped SAVEGAME reads without error: " + in_quotes(error));
    check(list.present_count() == 2, "the shipped install has two saves");
    check(list.at(0).present && list.at(0).name == "A SAVED GAME",
          "saved0 title: " + in_quotes(list.at(0).name));
    check(list.at(0).level == 0 && list.at(0).mission == 1,
          "saved0 level 0, mission F2 = 1");
    check(list.at(1).present && list.at(1).name == "2", "saved1 title: " + in_quotes(list.at(1).name));
    check(list.at(1).level == 0 && list.at(1).mission == 1, "saved1 level 0, mission 1");
    check(list.item_label(0) == "A SAVED GAME" && list.item_label(2) == gm::SaveSlotList::kEmptyLabel,
          "real rows 0/2 are name and placeholder");
    std::printf("save_slots: shipped install -> present=2 saved0=\"A SAVED GAME\" saved1=\"2\"\n");
}

} // namespace

int main() {
    test_empty();
    test_one_and_header_fields();
    test_six_slots();
    test_scroll_bounds();
    test_selection_window();
    test_overwrite();
    test_corrupt_and_determinism();
    test_installed_saves();

    if (failures == 0) {
        std::printf("save_slots: %d checks, 20 slots, %d visible, scroll 0..13\n", checks,
                    gm::SaveSlotList::kVisible);
    }
    return failures == 0 ? 0 : 1;
}
