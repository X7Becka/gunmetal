#pragma once

// Headless front-end menu state machine: the shell of `gundx.exe` without a
// window, without rendering and without reading the installation.
//
// This is the C++ counterpart of the screen map in docs/UI_SCREENS.md, which is
// the source of truth for every number here:
//
//   * §1 - the state dword `0x4bc440`, the jump table `0x4883d8` (ids 5..37) and
//          the driver states 0..4 that are not in the table;
//   * §2 - the screen map (id, handler, `rows`/`step`, title, title VA);
//   * §4 - the transitions, 35 table rows byte-checked at the `mov dword
//          [0x4bc440], imm` sites.
//
// What this class owns: the current screen, the selected item, the current
// key-set slots, and the visible item list of the current screen.  What it does
// not own: pixels, fonts, atlases (`FRONTEND/BMP.IDX`), the `.sml`/`.sme` pager,
// save files, `KEYCFG` profiles, UI sounds (`updown`/`select`/`selectm`), and
// the keyboard itself - the caller feeds `KeyEvent`s.
//
// Invariants
// ----------
//   * Before `reset()` (and in `Screen::None`/`Screen::Exit`) `feed()` is a safe
//     no-op: it returns false and changes nothing; `items()` is empty and
//     `back()` returns the current screen.
//   * An event that no §4 row consumes changes nothing and returns false.  The
//     one exception is the one-frame alias 34 -> 35 (`0x48e212`, §1): the alias
//     is applied at the start of the next `feed()`, independently of the key,
//     exactly as the game rewrites the state before the next input pass.
//   * `Escape` on `Screen::Main` goes to `Screen::Quit` (task invariant; §4 has
//     no row for it - the original only handles Esc in the sub-screens).
//   * Item lists are a static snapshot of §2/§3: the game composes several of
//     them per frame from globals (save slots, key-set names, joystick names),
//     and none of that data is loaded here.
//
// See docs/FRONTEND.md for the tables, the evidence classes and the limits.

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace gm {

/// Screens of the front-end.  The numeric value is the id of
/// docs/UI_SCREENS.md §2 (`[0x4bc440]`); ids 7, 8, 28, 29 are deliberately
/// absent: §1/§2 describe them as transitions without a handler (they jump
/// straight to the common epilogue `0x487f64`), so they are not screens.
///
/// `None` is "no screen yet" (before `reset()`), `Exit` is the terminal state
/// of the automaton: the original leaves the front-end from the caller's loop,
/// not by writing `[0x4bc440]`, so it has no §2 id.
enum class Screen : int {
    None = -1,
    // §1: driver states 0..4, handled outside the jump table.
    Intro = 0,                 ///< intro video, `0x499fc0`
    Main = 1,                  ///< title carousel `0x486740..0x48694f`, 7 entries
    SmlPager = 2,              ///< SML pager (help/credits/briefing), `0x487b69`
    BriefingTransitionA = 3,   ///< briefing transition `0x4882d6` (not ordered in §1)
    BriefingTransitionB = 4,   ///< briefing transition `0x4882d1`
    // §2: ids 5..37 (7, 8, 28, 29 are handler-less transitions, see above).
    Error = 5,                 ///< `ERROR`, handler `0x48acd0`
    Restart = 6,               ///< `RESTART GAME`, `0x488850` (also the game-over menu)
    InGameMenu = 9,            ///< in-game menu / level state, no handler
    ControlOptions = 10,       ///< `CONTROL OPTIONS`, `0x488ba0`
    SoundOptions = 11,         ///< `SOUND OPTIONS`, `0x4889f0`
    GraphicsOptions = 12,      ///< `GRAPHICS OPTIONS`, `0x485ee0`
    StartGame = 13,            ///< `START GAME`, `0x48aa60`
    SaveGame = 14,             ///< `SAVE GAME`, `0x48abd0`
    SaveNameEntry = 15,        ///< save name entry, no handler, buffer `0x661280`
    KeyJamming = 16,           ///< `KEY JAMMING`, `0x488e90`
    LevelSelect = 17,          ///< `SELECT LEVEL FOR NEW GAME`, `0x48a3c0`
    VideoMode = 18,            ///< `SELECT VIDEO MODE`, `0x48a8b0`
    MultiplayerType = 19,      ///< `MULTIPLAYER GAME TYPE`, `0x48a580`
    Shareware = 20,            ///< `LAY DOWN THE BUCKS`, `0x48a830`
    LocalPlayers = 21,         ///< `NUMBER OF LOCAL PLAYERS`, `0x48a780`
    MouseSetup = 22,           ///< `MOUSE SETUP`, `0x489a10`
    Difficulty = 23,           ///< `SET DIFFICULTY LEVEL`, `0x48a9a0`
    PlayerOneName = 24,        ///< `LOCAL PLAYER ONE NAME`, `0x489020(0)`
    PlayerTwoName = 25,        ///< `LOCAL PLAYER TWO NAME`, `0x489020(1)`
    JoystickCentering = 26,    ///< `JOYSTICK CENTERING`, `0x488f70`
    MultiplayerOptions = 27,   ///< `MULTIPLAY GAME OPTIONS`, `0x4885c0`
    KeySetView = 30,           ///< key set view, `0x489850`, title = set name
    NewKeySetName = 31,        ///< `NAME OF NEW KEY SET`, dialog `0x48ebb0`
    PlayOptions = 32,          ///< `PLAY OPTIONS`, `0x486170`
    Quit = 33,                 ///< `QUIT GAME?` / `QUIT OPTIONS`, `0x488780`
    KeySetEditorEntry = 34,    ///< one-frame alias of 35, `0x48e212`
    KeySetEditor = 35,         ///< key set editor, `0x4892d0`
    JoystickSetup = 36,        ///< `PLAYER 1/2 JOYSTICK SETUP`, `0x489d60`
    NewVideoMode = 37,         ///< `NEW VIDEO MODE OK?`, `0x488990`
    Exit = 100,                ///< terminal state, not a §2 id
};

/// One input event.  `code` is the raw key code when the caller has one; this
/// automaton only looks at `kind`, so unknown codes never move the state.
struct KeyEvent {
    enum Kind { Up, Down, Left, Right, Enter, Escape, Backspace, Text, Other };

    Kind kind = Other;
    int code = 0;
};

/// One visible menu item of a screen.
///
/// `label` is the engine string as written in the binary (`%s`, `%ld` are
/// filled at runtime from globals and stay literal here), `va` is its VA in
/// `gundx.exe` (`"-"` when the label is composed or comes from a table);
/// `id` is the item id the screen's dispatcher compares (`-1` = plain text,
/// the `item == -1` case of §3 that is never selectable); `x`/`row` are the
/// arguments of the item renderer `0x485dd0` (§3).
struct Item {
    std::string_view label;
    int id = -1;
    int x = 0;
    int row = 0;
    std::string_view va = "-";

    bool selectable() const { return id >= 0; }
};

/// What kind of input a transition row consumes.
enum class Rule {
    ItemEnter,     ///< Enter on the item with this `id`
    Escape,        ///< Esc from this screen (§4 "Esc ...")
    IntroEnd,      ///< intro finished or skipped: Enter/Esc (§4 row 1)
    KeysetAlias,   ///< 34 -> 35 on the next input pass, `0x48e212` (§1, §4 row 33)
    KeysetNewName, ///< entering a key set whose name is `NEW SET` -> state 31 (§7)
};

/// One row of the §4 transition table (or one screen duplicate of it).
struct Transition {
    Screen from = Screen::None;
    Screen to = Screen::None;
    Rule rule = Rule::Escape;
    int item = -1;            ///< Rule::ItemEnter only
    int doc_row = 0;          ///< 1-based row number in docs/UI_SCREENS.md §4
    std::string_view site = {}; ///< site from §4 (`mov dword [0x4bc440], imm`)
    bool enabled = true;      ///< false: struck-out row (9 -> 1, credits)
};

/// An edge that is *not* in the §4 table but is needed to reach every §2 screen
/// from the documented entry points.  `evidence` says where it comes from; the
/// automaton never applies these - they exist for the reachability report.
struct EntryEdge {
    Screen from = Screen::None;
    Screen to = Screen::None;
    std::string_view evidence = {};
};

/// One `KEYCFG` slot (§7: at most 16 sets, name table `0x6c2058 + set*0x108`).
struct KeySet {
    std::string_view name = "NEW SET"; ///< §7: a set with this name asks for a name first
    bool named = false;
};

class FrontendMenu {
public:
    /// No screen until `reset()`: `feed()` is a safe no-op (see the header).
    FrontendMenu() = default;

    /// Start a fresh session at `Screen::Main` (the intro video is not played;
    /// use `enter(Screen::Intro)` to rehearse it).  Clears the selection and
    /// restores the shipped key-set slots.
    void reset();

    Screen screen() const { return screen_; }

    /// Apply one key event.  Returns true when the observable state (screen,
    /// selection or key-set slot) changed.  Unknown keys and keys no §4 row
    /// consumes return false and leave the state alone.
    bool feed(const KeyEvent& event);

    /// Visible items of the current screen (static snapshot, see the header).
    const std::vector<Item>& items() const { return items_; }

    /// Screen Esc would move to (§4 row for this screen, `MAIN -> QUIT` for the
    /// title).  Screens without a documented Esc row return themselves.
    Screen back() const;

    /// Index into `items()` of the highlighted row (0 when the screen has no
    /// selectable item).  Several rows may share one item id (the two lines of
    /// a player count, the wrapped play options): the engine highlights by id
    /// (§3), so every row with this id is part of the same selection.
    std::size_t selected() const { return cursor_; }

    /// Id of the highlighted item, -1 when nothing is selectable.
    int selected_id() const;

    /// Current key set slot (0..15) of `player` 1 or 2 (§7: `[0x5a4910]` and
    /// `[0x4aa488]`); out-of-range players read player 1.
    int key_set(int player) const;

    /// True when slot *slot* is still the unnamed `NEW SET` (§7).
    bool key_set_unnamed(int slot) const;

    /// Jump to a screen by id.  This is a host/debug entry, not a §4 edge; it
    /// resets the selection.  `Screen::None` is refused.
    bool enter(Screen screen);

    // --- static tables (also used by the test and docs/FRONTEND.md) ---------

    /// All 34 screens of §2 (ids ascending, no `None`/`Exit`).
    static std::span<const Screen> screens();

    /// The §4 table: 36 entries for 35 rows (row 34 covers 34/35 -> 10, so it is
    /// stored twice; row 13, the struck-out credits row, is stored disabled).
    static std::span<const Transition> transitions();

    /// Analysis-only edges used by the reachability report (never applied).
    static std::span<const EntryEdge> entry_edges();

    /// Documented entry points: the app starts in the intro, the in-game menu is
    /// opened while playing (§4 row 11), ERROR is raised by the engine (§2).
    static std::span<const Screen> roots();

    /// §2 title of a screen (for `MAIN`/`INTRO`/the driver states the doc has no
    /// engine string, so the label is the doc name in capitals).
    static std::string_view screen_name(Screen screen);

    /// §2 id of a screen, or -1 for `None`/`Exit`.
    static int screen_id(Screen screen);

private:
    void set_screen(Screen screen);
    void rebuild_items();
    bool move_cursor(int delta);
    bool adjust(int delta);
    bool enter_pressed();
    bool escape_pressed();
    bool open_key_set(int player);
    const Transition* item_row(Screen screen, int item) const;

    Screen screen_ = Screen::None;
    std::size_t cursor_ = 0;
    std::array<int, 2> key_set_{0, 1}; ///< §7 defaults: PERFECT (0), SPLIT1 (1)
    std::array<KeySet, 16> key_sets_{};
    std::vector<Item> items_;
};

} // namespace gm
