// Headless front-end menu automaton (docs/UI_SCREENS.md §1, §2, §4).
//
// No game installation is touched: every expectation comes from the tables in
// docs/UI_SCREENS.md.  The checks:
//
//   * the §2 screen list (34 screens, ids 0..37 without the four handler-less
//     transitions 7/8/28/29) and its names;
//   * the §4 transition table: 35 rows -> 36 stored entries (row 34 covers both
//     34 and 35, row 13 is the struck-out credits edge), and every enabled entry
//     actually executes from its own screen;
//   * reachability (BFS): §4 alone from MAIN, then the full graph with the
//     documented entry edges - the second one must reach every screen;
//   * Escape/`back()` per §4, `Escape` on MAIN -> QUIT, Enter on QUIT -> exit;
//   * determinism (the same key string gives the same screen chain);
//   * negatives: no `reset()`, unknown key codes, empty event, screens with no
//     documented Esc row, the terminal state.
//
// Numbers are printed at the end (screens, §4 rows, BFS paths) so the report in
// docs/FRONTEND.md can quote them.

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "ui/frontend_menu.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

gm::KeyEvent key(gm::KeyEvent::Kind kind, int code = 0) {
    gm::KeyEvent event;
    event.kind = kind;
    event.code = code;
    return event;
}

bool press(gm::FrontendMenu& menu, gm::KeyEvent::Kind kind) {
    return menu.feed(key(kind));
}

std::string names(const std::vector<gm::Screen>& screens) {
    std::string out;
    for (gm::Screen screen : screens) {
        if (!out.empty()) {
            out += ' ';
        }
        out += gm::FrontendMenu::screen_name(screen);
    }
    return out;
}

/// Select the item with `id` by pressing Down (the cursor wraps, so 64 steps
/// cover any screen).
bool select_item(gm::FrontendMenu& menu, int id) {
    for (int i = 0; i < 64 && menu.selected_id() != id; ++i) {
        press(menu, gm::KeyEvent::Down);
    }
    return menu.selected_id() == id;
}

/// A: §2 screens and names.
void test_screens() {
    const auto screens = gm::FrontendMenu::screens();
    check(screens.size() == 34, "§2 has 34 screens, got " + std::to_string(screens.size()));

    std::set<int> ids;
    std::set<std::string> names_seen;
    for (gm::Screen screen : screens) {
        const int id = gm::FrontendMenu::screen_id(screen);
        check(id >= 0 && id <= 37, "screen id in 0..37");
        check(ids.insert(id).second, "screen id " + std::to_string(id) + " is unique");
        const std::string_view name = gm::FrontendMenu::screen_name(screen);
        check(!name.empty() && name != "(unknown)", "screen " + std::to_string(id) + " has a name");
        check(names_seen.insert(std::string(name)).second, "screen name is unique");
        check(gm::FrontendMenu::screen_id(gm::Screen::None) == -1, "None has no id");
        check(gm::FrontendMenu::screen_id(gm::Screen::Exit) == -1, "Exit has no id");
    }
    check(!ids.count(7) && !ids.count(8) && !ids.count(28) && !ids.count(29),
          "7/8/28/29 are handler-less transitions, not screens (§1)");
    check(gm::FrontendMenu::screen_id(gm::Screen::Main) == 1, "MAIN is state 1");
    check(gm::FrontendMenu::screen_id(gm::Screen::ControlOptions) == 10, "CONTROL OPTIONS is 10");
    check(gm::FrontendMenu::screen_id(gm::Screen::Quit) == 33, "QUIT is 33");
    check(gm::FrontendMenu::screen_id(gm::Screen::KeySetEditor) == 35, "editor is 35");
    check(gm::FrontendMenu::screen_name(gm::Screen::None) == "(none)", "None name");
    check(gm::FrontendMenu::screen_name(gm::Screen::Exit) == "EXIT", "Exit name");
}

/// Run one §4 entry from its own screen.  `how` receives a note for the report.
bool run_transition(const gm::Transition& row, std::string& how) {
    gm::FrontendMenu menu;
    menu.reset();
    check(menu.enter(row.from), "enter(" + std::string(gm::FrontendMenu::screen_name(row.from)) +
                                    ") is accepted");
    switch (row.rule) {
    case gm::Rule::ItemEnter:
        how = "Enter on item " + std::to_string(row.item);
        if (!select_item(menu, row.item)) {
            how += " (item not selectable)";
            return false;
        }
        press(menu, gm::KeyEvent::Enter);
        break;
    case gm::Rule::Escape:
        how = "Esc";
        press(menu, gm::KeyEvent::Escape);
        break;
    case gm::Rule::IntroEnd:
        how = "Enter skips the intro";
        press(menu, gm::KeyEvent::Enter);
        break;
    case gm::Rule::KeysetAlias:
        how = "next input pass (any key)";
        press(menu, gm::KeyEvent::Other);
        break;
    case gm::Rule::KeysetNewName: {
        // §7: a set named NEW SET asks for the name first.  P1 starts on slot 0
        // (PERFECT); 15 Right presses reach the last unnamed slot.
        how = "Right x15 (unnamed slot) + Enter on item 1";
        for (int i = 0; i < 15; ++i) {
            press(menu, gm::KeyEvent::Right);
        }
        if (menu.key_set(1) != 15 || !menu.key_set_unnamed(15)) {
            how += " (slot not unnamed)";
            return false;
        }
        if (!select_item(menu, 1)) {
            return false;
        }
        press(menu, gm::KeyEvent::Enter);
        break;
    }
    }
    return menu.screen() == row.to;
}

/// B: the §4 table, row by row.
void test_transitions() {
    const auto rows = gm::FrontendMenu::transitions();
    check(rows.size() == 36, "§4 stores 36 entries, got " + std::to_string(rows.size()));

    std::set<int> doc_rows;
    int enabled = 0;
    int disabled = 0;
    for (const gm::Transition& row : rows) {
        check(row.doc_row >= 1 && row.doc_row <= 35, "§4 row number in 1..35");
        doc_rows.insert(row.doc_row);
        if (row.enabled) {
            ++enabled;
        } else {
            ++disabled;
        }
        if (!row.enabled) {
            continue;
        }
        std::string how;
        const bool ok = run_transition(row, how);
        check(ok, "§4 row " + std::to_string(row.doc_row) + " (" +
                      std::string(gm::FrontendMenu::screen_name(row.from)) + " -> " +
                      std::string(gm::FrontendMenu::screen_name(row.to)) + ", " + how + ", " +
                      std::string(row.site) + ") executes");
        check(row.site.size() >= 6, "§4 row " + std::to_string(row.doc_row) + " has a site VA");
    }
    check(doc_rows.size() == 35, "all 35 §4 rows are represented");
    for (int i = 1; i <= 35; ++i) {
        check(doc_rows.count(i) == 1, "§4 row " + std::to_string(i) + " exists once");
    }
    check(disabled == 1, "exactly one disabled row (the struck-out credits edge)");
    check(enabled == 35, "35 enabled §4 entries, got " + std::to_string(enabled));

    // The one disabled row is the credits edge 9 -> 1 of §4 row 13.
    const gm::Transition* credits = nullptr;
    for (const gm::Transition& row : rows) {
        if (row.doc_row == 13) {
            credits = &row;
        }
    }
    check(credits != nullptr && !credits->enabled && credits->from == gm::Screen::InGameMenu &&
              credits->to == gm::Screen::Main && credits->site == "0x48347e",
          "row 13 is 9 -> 1 at 0x48347e and is disabled");

    // Row 20 has two targets (§4 "30 / 34"): set 0 is View, a named set is the
    // editor, an unnamed set is the name dialog (§7).
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::ControlOptions);
        check(menu.key_set(1) == 0, "player 1 starts on set 0 (PERFECT)");
        select_item(menu, 1);
        press(menu, gm::KeyEvent::Enter);
        check(menu.screen() == gm::Screen::KeySetView, "set 0 opens as the read-only view");
    }
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::ControlOptions);
        press(menu, gm::KeyEvent::Right); // set 0 -> 1 (SPLIT1, named)
        select_item(menu, 1);
        press(menu, gm::KeyEvent::Enter);
        check(menu.screen() == gm::Screen::KeySetEditorEntry, "named set != 0 opens the editor");
        check(press(menu, gm::KeyEvent::Other), "the alias is applied on the next input pass");
        check(menu.screen() == gm::Screen::KeySetEditor, "34 -> 35 (0x48e212)");
    }
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::ControlOptions);
        check(menu.key_set(2) == 1, "player 2 starts on set 1 (SPLIT1)");
        select_item(menu, 3);
        press(menu, gm::KeyEvent::Enter);
        check(menu.screen() == gm::Screen::KeySetEditorEntry, "item 3 (player 2) opens the editor");
    }
}

/// C: reachability.  Returns the reachable set; `path` (when not null) receives
/// a BFS tree for printing.
std::set<gm::Screen> bfs(const std::vector<gm::Screen>& start, bool with_entry_edges,
                         std::map<gm::Screen, gm::Screen>* parent = nullptr) {
    std::set<gm::Screen> seen(start.begin(), start.end());
    std::queue<gm::Screen> queue;
    for (gm::Screen screen : start) {
        queue.push(screen);
        if (parent != nullptr) {
            (*parent)[screen] = gm::Screen::None;
        }
    }
    while (!queue.empty()) {
        const gm::Screen screen = queue.front();
        queue.pop();
        std::vector<gm::Screen> next;
        for (const gm::Transition& row : gm::FrontendMenu::transitions()) {
            if (row.enabled && row.from == screen) {
                next.push_back(row.to);
            }
        }
        if (with_entry_edges) {
            for (const gm::EntryEdge& edge : gm::FrontendMenu::entry_edges()) {
                if (edge.from == screen) {
                    next.push_back(edge.to);
                }
            }
        }
        for (gm::Screen to : next) {
            if (seen.insert(to).second) {
                if (parent != nullptr) {
                    (*parent)[to] = screen;
                }
                queue.push(to);
            }
        }
    }
    return seen;
}

std::vector<gm::Screen> all_screens() {
    const auto screens = gm::FrontendMenu::screens();
    return std::vector<gm::Screen>(screens.begin(), screens.end());
}

void report_unreachable(const std::set<gm::Screen>& seen) {
    std::vector<gm::Screen> missing;
    for (gm::Screen screen : all_screens()) {
        if (!seen.count(screen)) {
            missing.push_back(screen);
        }
    }
    std::cout << "  unreachable (" << missing.size() << "): ";
    if (missing.empty()) {
        std::cout << "none";
    }
    for (gm::Screen screen : missing) {
        std::cout << gm::FrontendMenu::screen_name(screen) << ' ';
    }
    std::cout << "\n";
}

std::string path_to(const std::map<gm::Screen, gm::Screen>& parent, gm::Screen target) {
    std::vector<gm::Screen> chain;
    gm::Screen at = target;
    while (at != gm::Screen::None) {
        chain.push_back(at);
        const auto it = parent.find(at);
        if (it == parent.end()) {
            break;
        }
        at = it->second;
    }
    std::reverse(chain.begin(), chain.end());
    return names(chain);
}

void test_reachability() {
    // §4 alone, from MAIN.  This is what the byte-checked table covers.
    const std::set<gm::Screen> proven = bfs({gm::Screen::Main}, false);
    std::cout << "BFS over §4 from MAIN: " << proven.size() << "/" << all_screens().size() << "\n";
    report_unreachable(proven);

    // Full graph: documented entry points (intro, in-game menu, ERROR) plus the
    // labelled entry edges of the wizard/video-mode/pager branches.
    std::vector<gm::Screen> roots(gm::FrontendMenu::roots().begin(),
                                  gm::FrontendMenu::roots().end());
    roots.push_back(gm::Screen::Main); // the title itself, the user-facing root
    std::map<gm::Screen, gm::Screen> parent;
    const std::set<gm::Screen> full = bfs(roots, true, &parent);
    std::cout << "BFS over the full graph: " << full.size() << "/" << all_screens().size() << "\n";
    report_unreachable(full);

    const std::size_t total = all_screens().size();
    check(full.size() == total, "the full graph reaches all " + std::to_string(total) + " screens");
    check(proven.size() < total, "§4 alone does not cover the entry-only screens");

    // Spot-check the documented chains (§4 prose: 21 -> 17 -> 23 -> 24).
    check(proven.count(gm::Screen::ControlOptions) == 1, "MAIN -> CONTROL OPTIONS is in §4");
    check(proven.count(gm::Screen::StartGame) == 1, "MAIN -> START GAME is in §4");
    check(proven.count(gm::Screen::PlayOptions) == 1, "MAIN -> PLAY OPTIONS is in §4");
    check(proven.count(gm::Screen::KeySetEditor) == 0, "the editor needs the 34 -> 35 alias edge");
    check(full.count(gm::Screen::KeySetEditor) == 1, "the editor is reachable in the full graph");

    std::cout << "BFS paths:\n";
    for (gm::Screen target : {gm::Screen::ControlOptions, gm::Screen::StartGame,
                              gm::Screen::PlayOptions, gm::Screen::KeySetEditor}) {
        std::cout << "  " << path_to(parent, target) << "\n";
    }
}

/// D: Escape/back() per §4 and the MAIN/QUIT rules.
void test_escape_and_quit() {
    // Every Escape row: back() predicts the same screen feed() moves to.
    int escape_entries = 0;
    std::set<int> escape_rows;
    for (const gm::Transition& row : gm::FrontendMenu::transitions()) {
        if (row.rule != gm::Rule::Escape || !row.enabled) {
            continue;
        }
        ++escape_entries;
        escape_rows.insert(row.doc_row);
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(row.from);
        check(menu.back() == row.to, "back() of " + std::string(gm::FrontendMenu::screen_name(row.from)) +
                                         " is " + std::string(gm::FrontendMenu::screen_name(row.to)));
        press(menu, gm::KeyEvent::Escape);
        check(menu.screen() == row.to, "Esc moves " + std::string(gm::FrontendMenu::screen_name(row.from)) +
                                           " -> " + std::string(gm::FrontendMenu::screen_name(row.to)));
    }
    // §4 has 10 Escape rows; row 34 ("34/35 -> 10") covers two screens.
    check(escape_rows.size() == 10,
          "10 Escape rows in §4, got " + std::to_string(escape_rows.size()));
    check(escape_entries == 11,
          "11 Escape entries (row 34 twice), got " + std::to_string(escape_entries));

    // MAIN has no §4 Esc row: the task invariant sends it to QUIT.
    gm::FrontendMenu menu;
    menu.reset();
    check(menu.screen() == gm::Screen::Main, "reset() starts at MAIN");
    check(menu.back() == gm::Screen::Quit, "back() on MAIN is QUIT");
    check(press(menu, gm::KeyEvent::Escape), "Esc on MAIN changes the state");
    check(menu.screen() == gm::Screen::Quit, "Esc on MAIN goes to QUIT");

    // QUIT: Yes -> exit, No -> title (§3.8, no §4 row).
    check(menu.selected_id() == 0, "QUIT highlights Yes first");
    check(press(menu, gm::KeyEvent::Enter), "Enter on Yes changes the state");
    check(menu.screen() == gm::Screen::Exit, "Yes leaves the front-end");
    check(!press(menu, gm::KeyEvent::Enter) && !press(menu, gm::KeyEvent::Escape),
          "the terminal state ignores keys");
    check(menu.back() == gm::Screen::Exit, "back() of the terminal state is itself");
    menu.reset();
    check(menu.screen() == gm::Screen::Main, "reset() leaves the terminal state");

    menu.enter(gm::Screen::Quit);
    select_item(menu, 1);
    press(menu, gm::KeyEvent::Enter);
    check(menu.screen() == gm::Screen::Main, "No returns to the title");

    // Screens with no documented Esc row stay put.
    for (gm::Screen screen : {gm::Screen::SoundOptions, gm::Screen::GraphicsOptions,
                              gm::Screen::PlayOptions, gm::Screen::MultiplayerType}) {
        gm::FrontendMenu stuck;
        stuck.reset();
        stuck.enter(screen);
        check(stuck.back() == screen, "back() without a §4 row returns the screen");
        check(!press(stuck, gm::KeyEvent::Escape), "Esc without a §4 row is a no-op");
        check(stuck.screen() == screen, "Esc without a §4 row keeps the screen");
    }
}

/// E: item snapshots.
void test_items() {
    gm::FrontendMenu menu;
    menu.reset();
    check(menu.items().size() == 7, "MAIN has 7 carousel entries");
    for (std::size_t i = 0; i < menu.items().size(); ++i) {
        check(menu.items()[i].id == static_cast<int>(i), "MAIN item ids are 0..6 in order");
        check(menu.items()[i].selectable(), "MAIN entries are selectable");
    }
    check(menu.selected() == 0 && menu.selected_id() == 0, "MAIN starts on entry 0");

    // Up/Down wrap around the carousel.
    press(menu, gm::KeyEvent::Up);
    check(menu.selected_id() == 6, "Up from entry 0 wraps to the last entry");
    press(menu, gm::KeyEvent::Down);
    check(menu.selected_id() == 0, "Down wraps back to entry 0");

    menu.enter(gm::Screen::ControlOptions);
    check(menu.items().size() == 10, "CONTROL OPTIONS has 10 items (§3.1)");
    check(menu.items().back().label == "Done" && menu.items().back().id == 9, "item 9 is Done");

    menu.enter(gm::Screen::KeySetView);
    check(menu.items().size() == 32, "KEY SET VIEW has 31 bindings + hint");
    check(menu.items()[0].label == "Move Forward", "binding 0 is Move Forward");
    check(menu.items()[30].label == "Linear Right", "binding 30 is Linear Right");
    check(!menu.items()[31].selectable(), "the hint is not selectable");

    menu.enter(gm::Screen::KeySetEditor);
    check(menu.items()[31].label == "Enter changes, Delete clears", "editor hint (§3.7)");
    check(menu.items()[31].va == "0x4bd7a8", "editor hint VA");

    // Two-line items share one id and one selection stop (§3.1/§3.4).
    menu.enter(gm::Screen::LocalPlayers);
    check(menu.items().size() == 5, "NUMBER OF LOCAL PLAYERS has five lines");
    press(menu, gm::KeyEvent::Down);
    check(menu.selected_id() == 1, "Down skips the second line of item 0");

    menu.enter(gm::Screen::PlayOptions);
    check(menu.items().size() == 11, "PLAY OPTIONS has 11 lines (§3.4/--json)");

    // Every §2 screen resolves a name; the menu screens expose a snapshot.
    for (gm::Screen screen : gm::FrontendMenu::screens()) {
        menu.enter(screen);
        check(menu.screen() == screen, "enter() lands on the requested screen");
        for (const gm::Item& item : menu.items()) {
            check(!item.label.empty(), "item labels are not empty");
            check(item.id >= -1, "item id is -1 or a menu id");
        }
    }
}

/// F: determinism.
void test_determinism() {
    const std::vector<gm::KeyEvent::Kind> script = {
        gm::KeyEvent::Down,      gm::KeyEvent::Down, gm::KeyEvent::Down,
        gm::KeyEvent::Enter,     gm::KeyEvent::Enter, gm::KeyEvent::Enter,
        gm::KeyEvent::Escape,    gm::KeyEvent::Enter, gm::KeyEvent::Backspace,
        gm::KeyEvent::Other,
    };

    const auto run = [&](std::vector<std::string>& chain, std::vector<int>& selection) {
        gm::FrontendMenu menu;
        menu.reset();
        chain.push_back(std::string(gm::FrontendMenu::screen_name(menu.screen())));
        selection.push_back(menu.selected_id());
        for (gm::KeyEvent::Kind kind : script) {
            menu.feed(key(kind));
            chain.push_back(std::string(gm::FrontendMenu::screen_name(menu.screen())));
            selection.push_back(menu.selected_id());
        }
    };

    std::vector<std::string> chain_a;
    std::vector<std::string> chain_b;
    std::vector<int> selection_a;
    std::vector<int> selection_b;
    run(chain_a, selection_a);
    run(chain_b, selection_b);
    check(chain_a == chain_b, "the same key string gives the same screen chain");
    check(selection_a == selection_b, "the same key string gives the same selection");

    std::cout << "  deterministic chain:";
    for (const std::string& name : chain_a) {
        std::cout << " -> " << name;
    }
    std::cout << "\n";

    // The script must actually exercise the tree, not sit on one screen.
    check(chain_a[4] == "START GAME", "Down x3 + Enter from MAIN opens START GAME");
    check(chain_a[5] == "SET DIFFICULTY LEVEL", "Enter opens the difficulty list");
    check(chain_a[6] == "LOCAL PLAYER ONE NAME", "Enter opens the player-one name");
    check(chain_a[7] == "LOCAL PLAYER ONE NAME", "Esc has no §4 row here");
    check(chain_a[8] == "LOCAL PLAYER ONE NAME", "Enter has nothing to activate here");
}

/// G: negatives.
void test_negatives() {
    // Before reset(): safe no-op, not a crash.
    gm::FrontendMenu fresh;
    check(fresh.screen() == gm::Screen::None, "a fresh menu is in None");
    check(fresh.items().empty(), "a fresh menu has no items");
    check(fresh.selected_id() == -1, "a fresh menu has no selection");
    check(fresh.back() == gm::Screen::None, "back() of None is None");
    check(!fresh.feed(key(gm::KeyEvent::Enter)), "Enter before reset() is refused");
    check(!fresh.feed(key(gm::KeyEvent::Escape)), "Esc before reset() is refused");
    check(!fresh.feed(key(gm::KeyEvent::Other, 0x98)), "unknown code before reset() is refused");
    check(fresh.screen() == gm::Screen::None, "state does not change before reset()");

    fresh.reset();
    const std::size_t before = fresh.selected();
    check(!fresh.feed(key(gm::KeyEvent::Other, 0x98)), "unknown code (up arrow) on MAIN is a no-op");
    check(!fresh.feed(key(gm::KeyEvent::Other, 999)), "unknown code 999 on MAIN is a no-op");
    check(!fresh.feed(key(gm::KeyEvent::Text, 'A')), "Text is a no-op");
    check(!fresh.feed(key(gm::KeyEvent::Backspace)), "Backspace is a no-op on MAIN");
    check(fresh.screen() == gm::Screen::Main && fresh.selected() == before,
          "no-op events leave screen and selection alone");

    // Empty event: default-constructed KeyEvent has Kind::Other.
    const gm::KeyEvent empty;
    check(!fresh.feed(empty), "an empty event is a no-op");

    // enter(None) is refused; a screen with no selectable item ignores Enter.
    check(!fresh.enter(gm::Screen::None), "enter(None) is refused");
    fresh.enter(gm::Screen::Shareware);
    check(fresh.selected_id() == -1, "id 20 has no selectable item");
    check(!fresh.feed(key(gm::KeyEvent::Enter)), "Enter on id 20 is a no-op");
    check(fresh.back() == gm::Screen::Shareware, "id 20 has no Esc row");

    // The automaton never leaves the §2 id range or invents a screen.
    gm::FrontendMenu loop;
    loop.reset();
    for (int i = 0; i < 500; ++i) {
        loop.feed(key(gm::KeyEvent::Down));
        loop.feed(key(gm::KeyEvent::Enter));
        loop.feed(key(gm::KeyEvent::Escape));
        check(loop.screen() != gm::Screen::None, "the automaton never falls back to None");
    }
}

} // namespace

int main() {
    test_screens();
    test_transitions();
    test_reachability();
    test_escape_and_quit();
    test_items();
    test_determinism();
    test_negatives();

    if (failures == 0) {
        int enabled = 0;
        std::set<int> doc_rows;
        for (const gm::Transition& row : gm::FrontendMenu::transitions()) {
            if (row.enabled) {
                ++enabled;
            }
            doc_rows.insert(row.doc_row);
        }
        std::cout << "frontend_menu: " << gm::FrontendMenu::screens().size() << " §2 screens, "
                  << doc_rows.size() << " §4 rows -> "
                  << gm::FrontendMenu::transitions().size() << " entries (" << enabled
                  << " enabled), " << gm::FrontendMenu::entry_edges().size()
                  << " documented entry edges\n";
    }
    return failures == 0 ? 0 : 1;
}
