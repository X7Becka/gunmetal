#include "ui/frontend_menu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>

namespace gm {
namespace {

// --- item snapshots -------------------------------------------------------
//
// Every label/VA/x/row below is copied from docs/UI_SCREENS.md §2/§3 (which
// parses the push arguments of the screen builders) or from `tools/re/
// ui_screens.py --json`.  Labels keep the `%s`/`%ld` placeholders the engine
// fills at runtime: this layer does not read save slots, `KEYCFG` names or
// joystick strings.

/// The 31 binding names of the action table `0x4bc558` (§7, `0x1f` items).
constexpr std::array<std::string_view, 31> kBindingNames = {
    "Move Forward",   "Move Backward",     "Turn Left",       "Turn Right",
    "Look Center",    "Fire",              "Look Up",         "Look Down",
    "Strafe",         "Strafe Left",       "Strafe Right",    "Throttle Up",
    "Throttle Down",  "Next Weapon",       "Previous Weapon", "Activate",
    "Next Equipment", "Previous Equip.",   "Toggle Equip. Use", "Headlights",
    "Force Field",    "Mine Detector",     "Targeting",       "Warp Device",
    "Radar",          "Mine Layer",        "Combat Block",    "Auto Sentry",
    "Decoy",          "Linear Left",       "Linear Right",
};

/// Key-set view (30) and editor (34/35) share the 31 binding rows: §3.7 gives
/// `x 5`, `row -29`, step 8, and a hint at row 20 with `x 38`.
constexpr std::array<Item, 32> make_binding_items(std::string_view hint,
                                                  std::string_view hint_va) {
    std::array<Item, 32> items{};
    for (std::size_t i = 0; i < kBindingNames.size(); ++i) {
        items[i] = Item{kBindingNames[i], static_cast<int>(i), 5, -29, "0x4bc558"};
    }
    items[31] = Item{hint, -1, 38, 20, hint_va};
    return items;
}

constexpr std::array<Item, 32> kKeySetViewItems =
    make_binding_items("Press Esc to close", "0x4bd7c8");
constexpr std::array<Item, 32> kKeySetEditorItems =
    make_binding_items("Enter changes, Delete clears", "0x4bd7a8");

/// §2: the title carousel is seven entry-art images (`sound`, `controls`,
/// `graphics`, `startgam`, `savegame`, `play`, `quitgame`); the order is the
/// order of the transitions `0x48cf78..0x48d019`.  There is no engine text.
constexpr std::array<Item, 7> kMainItems = {{
    {"sound", 0, 0, 0, "-"},   {"controls", 1, 0, 0, "-"}, {"graphics", 2, 0, 0, "-"},
    {"startgam", 3, 0, 0, "-"}, {"savegame", 4, 0, 0, "-"}, {"play", 5, 0, 0, "-"},
    {"quitgame", 6, 0, 0, "-"},
}};

constexpr std::array<Item, 1> kErrorItems = {{
    {"ESC TO RETURN TO MENU", -1, 100, 91, "0x4bdc38"},
}};

/// §4/`docs/GAME_OVER.md` §4.1: items 0..2 are gated at runtime, so the shipped
/// screen may show only `Quit to title`; the snapshot lists all four.
constexpr std::array<Item, 4> kRestartItems = {{
    {"Reload last saved game", 0, 10, 27, "0x4bd44c"},
    {"Restart level with same R.P.V.", 1, 10, 27, "0x4bd42c"},
    {"Restart level from HQ", 2, 10, 27, "0x4bd3fc"},
    {"Quit to title", 3, 10, 27, "0x4bd3ec"},
}};

/// §2 gives no item list for id 9 (in-game menu); §4 knows only item 0, which
/// restarts the level and writes state 0 (`0x485c50`).  The label is therefore
/// a placeholder, not an engine string.
constexpr std::array<Item, 1> kInGameItems = {{
    {"(item 0: restart level)", 0, 0, 0, "-"},
}};

constexpr std::array<Item, 10> kControlItems = {{
    {"Player 1: %s", 0, 20, 27, "0x4bd5e4"},
    {"Assign %s", 1, 32, 27, "0x4bd5d0"},
    {"Player 2: %s", 2, 20, 27, "0x4bd5c0"},
    {"Assign %s", 3, 32, 27, "0x4bd5d0"},
    {"Keyjamming", 4, 20, 27, "0x4bd5b4"},
    {"1: %.25s", 5, 20, 27, "0x4bd58c"},
    {"2: %.25s", 6, 20, 27, "0x4bd564"},
    {"Joystick Centering", 7, 20, 27, "0x4bd550"},
    {"Mouse: %s", 8, 20, 27, "0x4bd544"},
    {"Done", 9, 20, 27, "0x4bceb4"},
}};

constexpr std::array<Item, 4> kSoundItems = {{
    {"Music Volume:", 0, 18, 27, "0x4bd508"},
    {"Effects Volume:", 2, 18, 27, "0x4bd4f8"},
    {"Switch Left Right", 4, 30, 27, "0x4bd4e4"},
    {"Done", 6, 18, 24, "0x4bceb4"},
}};

constexpr std::array<Item, 6> kGraphicsItems = {{
    {"(video mode)", 0, 38, 24, "0x4c05c0"},
    {"Border:", 1, 38, 24, "0x4bcf1c"},
    {"%s Gamma Correction", 2, 38, 24, "0x4bcf08"},
    {"Reflections", 3, 50, 24, "0x4bcefc"},
    {"Light Maps Off", 4, 38, 24, "0x4bcebc"},
    {"Done", 5, 38, 23, "0x4bceb4"},
}};

/// §3.6: items 2..7 are six save slots with rows -44, -55 ... -99; the snapshot
/// shows one slot row (id 2) and notes the missing six.
constexpr std::array<Item, 4> kStartGameItems = {{
    {"New Single Player Game", 0, 35, 23, "0x4bdc08"},
    {"New Multiplayer Game", 1, 35, -33, "0x4bdbf0"},
    {"(save slot name)", 2, 39, -44, "0x6c2fe0"},
    {"ESC TO CANCEL", -1, 35, -110, "0x4bd9d8"},
}};

constexpr std::array<Item, 2> kSaveGameItems = {{
    {"(save slot name)", 0, 42, -28, "0x6c2fe0"},
    {"ESC TO CANCEL", -1, 38, 101, "0x4bd9d8"},
}};

constexpr std::array<Item, 2> kLevelSelectItems = {{
    {"(level name)", 0, 14, -27, "0x68cda8"},
    {"ESC TO CANCEL", -1, 38, -111, "0x4bd9d8"},
}};

constexpr std::array<Item, 1> kVideoModeItems = {{
    {"Press Esc to close", -1, 25, 20, "0x4bd7c8"},
}};

constexpr std::array<Item, 12> kMultiplayerTypeItems = {{
    {"One Computer", 0, 18, -18, "0x4bdad0"},
    {"Network Client", 1, 18, -26, "0x4bdac0"},
    {"Network Server", 2, 18, -34, "0x4bdab0"},
    {"Network No Render Server", 3, 18, -42, "0x4bda94"},
    {"TCP/IP Client", 4, 18, -50, "0x4bda84"},
    {"TCP/IP Server", 5, 18, -58, "0x4bda74"},
    {"TCP/IP No Render Server", 6, 18, -66, "0x4bda5c"},
    {"Serial Cable Client", 7, 18, -74, "0x4bda48"},
    {"Serial Cable Server", 8, 18, -82, "0x4bda34"},
    {"Modem Client", 9, 18, -90, "0x4bda24"},
    {"Modem Server", 10, 18, -98, "0x4bda14"},
    {"Cancel", 11, 18, -106, "0x4bda0c"},
}};

/// §2/`--json`: id 20 has three plain text lines, no selectable item.
constexpr std::array<Item, 3> kSharewareItems = {{
    {"THIS FEATURE IS NOT", -1, 42, 27, "0x4bdb6c"},
    {"AVAILABLE IN THE", -1, 42, 36, "0x4bdb58"},
    {"SHAREWARE VERSION", -1, 42, 45, "0x4bdb44"},
}};

constexpr std::array<Item, 5> kLocalPlayersItems = {{
    {"One Player", 0, 38, 27, "0x4bdb20"},
    {"Full Screen", 0, 38, 36, "0x4bdb14"},
    {"Two Players", 1, 38, 27, "0x4bdb08"},
    {"Split Screen", 1, 38, 36, "0x4bdaf8"},
    {"Cancel", 2, 38, 27, "0x4bda0c"},
}};

constexpr std::array<Item, 12> kMouseItems = {{
    {"Used By: %s", 0, 18, 24, "0x4bd8b4"},
    {"Left/Right: %s", 1, 9, 24, "0x4bd8a4"},
    {"Sensitivity:", 2, 9, 24, "0x4bd894"},
    {"Forward/Back: %s", 3, 9, 24, "0x4bd880"},
    {"Sensitivity:", 4, 9, 24, "0x4bd894"},
    {"Up/Down (3D): %s", 5, 9, 24, "0x4bd86c"},
    {"Sensitivity:", 6, 9, 24, "0x4bd894"},
    {"Button 1: %s", 7, 9, 24, "0x4bd85c"},
    {"Button 2: %s", 8, 9, 24, "0x4bd838"},
    {"Button 3: %s", 9, 9, 24, "0x4bd814"},
    {"Button 4: %s", 10, 9, 24, "0x4bd7f0"},
    {"Done", 11, 9, 24, "0x4bceb4"},
}};

constexpr std::array<Item, 6> kDifficultyItems = {{
    {"Private", 0, 40, 27, "0x4bdbd0"},
    {"Corporal", 1, 40, 27, "0x4bdbc4"},
    {"Lieutenant", 2, 40, 27, "0x4bdbb8"},
    {"Captain", 3, 40, 27, "0x4bdbb0"},
    {"Major", 4, 40, 27, "0x4bdba8"},
    {"Cancel", 5, 40, 27, "0x4bda0c"},
}};

constexpr std::array<Item, 6> kPlayerOneNameItems = {{
    {"TYPE THE NAME YOU", -1, 16, 48, "0x4bd71c"},
    {"WISH TO USE", -1, 16, 56, "0x4bd710"},
    {"LEAVE BLANK TO USE A", -1, 16, 67, "0x4bd6f8"},
    {"RANDOM NAME", -1, 16, 75, "0x4bd6ec"},
    {"PRESS ENTER TO CONTINUE", -1, 16, 89, "0x4bd6d4"},
    {"PRESS ESC TO ABORT", -1, 16, 102, "0x4bd6c0"},
}};

constexpr std::array<Item, 5> kCenteringItems = {{
    {"Spiral the stick of the", 0, 36, 25, "0x4bd694"},
    {"joystick through all of", 0, 36, 35, "0x4bd67c"},
    {"it's positions.", 0, 36, 45, "0x4bd66c"},
    {"Press Esc or Enter", 0, 41, 86, "0x4bd658"},
    {"to close", 0, 41, 97, "0x4bd64c"},
}};

constexpr std::array<Item, 8> kMultiplayerOptionsItems = {{
    {"(deathmatch type)", 0, 23, 22, "0x4bc6e8"},
    {"Starting Cash: %ld", 1, 23, 16, "0x4bd374"},
    {"Teamplay", 2, 35, 13, "0x4bd368"},
    {"No Enemies", 3, 35, 12, "0x4bd35c"},
    {"Can't Buy Ammo", 4, 35, 11, "0x4bd34c"},
    {"Handicap Better Players", 5, 35, 10, "0x4bd334"},
    {"Allow Mouse Cheating", 6, 35, 7, "0x4bd31c"},
    {"START GAME!", 7, 23, -113, "0x4bd310"},
}};

constexpr std::array<Item, 11> kPlayOptionsItems = {{
    {"Skip Empty Weapons", 0, 25, 21, "0x4bd02c"},
    {"Skip Deadly Weapons", 1, 25, 17, "0x4bd018"},
    {"Switch Weapon To Picked-Up", 2, 25, 13, "0x4bcffc"},
    {"Ammo", 2, 25, 21, "0x4bcff4"},
    {"Auto Switch Weapons When", 3, 25, 15, "0x4bcfd8"},
    {"Ammo Depleted", 3, 25, 23, "0x4bcfc8"},
    {"Don't Auto-Target when", 4, 25, 18, "0x4bcfb0"},
    {"Looking", 4, 25, 26, "0x4bcfa8"},
    {"Mouse Cheating", 5, 25, 20, "0x4bcf98"},
    {"Dynamic Camera: None", 6, 13, 15, "0x4bcf38"},
    {"Done", 7, 13, 9, "0x4bceb4"},
}};

/// §12.4: id 33 is `QUIT GAME?` outside the game and `QUIT OPTIONS` inside it;
/// the snapshot is the outside-game variant (§3.8).
constexpr std::array<Item, 2> kQuitItems = {{
    {"Yes", 0, 38, 27, "0x4bd3dc"},
    {"No", 1, 38, 27, "0x4bc7a4"},
}};

/// §3.8 says `Yes`/`No`; `--json` finds only `Yes` (the `No` label `0x4bc7a4`
/// is shared with other screens).
constexpr std::array<Item, 2> kNewVideoModeItems = {{
    {"Yes", 0, 38, 27, "0x4bd3dc"},
    {"No", 1, 38, 27, "0x4bc7a4"},
}};

constexpr std::array<Item, 1> kJoystickSetupItems = {{
    {"Esc Exit, Button To Highlight", -1, 20, 20, "0x4bd988"},
}};

std::span<const Item> items_for(Screen screen) {
    switch (screen) {
    case Screen::Main: return kMainItems;
    case Screen::Error: return kErrorItems;
    case Screen::Restart: return kRestartItems;
    case Screen::InGameMenu: return kInGameItems;
    case Screen::ControlOptions: return kControlItems;
    case Screen::SoundOptions: return kSoundItems;
    case Screen::GraphicsOptions: return kGraphicsItems;
    case Screen::StartGame: return kStartGameItems;
    case Screen::SaveGame: return kSaveGameItems;
    case Screen::LevelSelect: return kLevelSelectItems;
    case Screen::VideoMode: return kVideoModeItems;
    case Screen::MultiplayerType: return kMultiplayerTypeItems;
    case Screen::Shareware: return kSharewareItems;
    case Screen::LocalPlayers: return kLocalPlayersItems;
    case Screen::MouseSetup: return kMouseItems;
    case Screen::Difficulty: return kDifficultyItems;
    case Screen::PlayerOneName: return kPlayerOneNameItems;
    case Screen::JoystickCentering: return kCenteringItems;
    case Screen::MultiplayerOptions: return kMultiplayerOptionsItems;
    case Screen::KeySetView: return kKeySetViewItems;
    case Screen::PlayOptions: return kPlayOptionsItems;
    case Screen::Quit: return kQuitItems;
    case Screen::KeySetEditorEntry:
    case Screen::KeySetEditor: return kKeySetEditorItems;
    case Screen::JoystickSetup: return kJoystickSetupItems;
    case Screen::NewVideoMode: return kNewVideoModeItems;
    // Driver states with no item list: intro, SML pager, the two briefing
    // transitions, the save-name buffer, key jamming, the player-two name
    // screen (§2 has no lines for it) and the new-key-set dialog.
    case Screen::None:
    case Screen::Exit:
    case Screen::Intro:
    case Screen::SmlPager:
    case Screen::BriefingTransitionA:
    case Screen::BriefingTransitionB:
    case Screen::SaveNameEntry:
    case Screen::KeyJamming:
    case Screen::PlayerTwoName:
    case Screen::NewKeySetName:
    default: return {};
    }
}

// --- §4 transition table --------------------------------------------------

/// One entry per row of docs/UI_SCREENS.md §4, in the order of the document
/// (`doc_row` 1..35).  Row 13 (credits from the in-game menu) is kept disabled:
/// the document strikes it out because `0x48347e` belongs to the VM action
/// handler `0x483470`, which no shipped script calls.  Row 34 (`34/35 -> 10`)
/// covers two source screens and is stored once per source.
constexpr Transition kTransitions[] = {
    // 1.  0 -> 1   интро закончилось/пропущено
    {Screen::Intro, Screen::Main, Rule::IntroEnd, -1, 1, "0x487709"},
    // 2.  1 -> 11  пункт 0 (sound art)
    {Screen::Main, Screen::SoundOptions, Rule::ItemEnter, 0, 2, "0x48cf78"},
    // 3.  1 -> 10  пункт 1 (controls art)
    {Screen::Main, Screen::ControlOptions, Rule::ItemEnter, 1, 3, "0x48cf8c"},
    // 4.  1 -> 12  пункт 2 (graphics art)
    {Screen::Main, Screen::GraphicsOptions, Rule::ItemEnter, 2, 4, "0x48cfa0"},
    // 5.  1 -> 13  пункт 3 (startgam art)
    {Screen::Main, Screen::StartGame, Rule::ItemEnter, 3, 5, "0x48cfb4"},
    // 6.  1 -> 14  пункт 4 (savegame art, только в игре)
    {Screen::Main, Screen::SaveGame, Rule::ItemEnter, 4, 6, "0x48cff1"},
    // 7.  1 -> 32  пункт 5 (play art)
    {Screen::Main, Screen::PlayOptions, Rule::ItemEnter, 5, 7, "0x48d005"},
    // 8.  1 -> 33  пункт 6 (quitgame art)
    {Screen::Main, Screen::Quit, Rule::ItemEnter, 6, 8, "0x48d019"},
    // 9.  5 -> 1   Esc из ERROR
    {Screen::Error, Screen::Main, Rule::Escape, -1, 9, "0x48b57a"},
    // 10. 6 -> 1   Quit to title (пункт 3, docs/GAME_OVER.md §4.1)
    {Screen::Restart, Screen::Main, Rule::ItemEnter, 3, 10, "0x48da31"},
    // 11. 9 -> 6   Esc открывает in-game menu
    {Screen::InGameMenu, Screen::Restart, Rule::Escape, -1, 11, "0x48b54c"},
    // 12. 9 -> 0   пункт 0 — рестарт уровня
    {Screen::InGameMenu, Screen::Intro, Rule::ItemEnter, 0, 12, "0x485c50"},
    // 13. 9 -> 1   ~~credits из in-game menu~~: вычеркнуто, обработчик VM-действия
    //    3 (0x483470) в поставочных скриптах не вызывается.
    {Screen::InGameMenu, Screen::Main, Rule::ItemEnter, -1, 13, "0x48347e", false},
    // 14. 10 -> 16  Keyjamming
    {Screen::ControlOptions, Screen::KeyJamming, Rule::ItemEnter, 4, 14, "0x48d1d9"},
    // 15. 10 -> 36  joystick setup, игрок 1
    {Screen::ControlOptions, Screen::JoystickSetup, Rule::ItemEnter, 5, 15, "0x48d1e4"},
    // 16. 10 -> 36  joystick setup, игрок 2
    {Screen::ControlOptions, Screen::JoystickSetup, Rule::ItemEnter, 6, 16, "0x48d209"},
    // 17. 10 -> 26  Joystick Centering
    {Screen::ControlOptions, Screen::JoystickCentering, Rule::ItemEnter, 7, 17, "0x48d26a"},
    // 18. 10 -> 22  Mouse Setup
    {Screen::ControlOptions, Screen::MouseSetup, Rule::ItemEnter, 8, 18, "0x48d292"},
    // 19. 10 -> 31  новый набор (очищает имя); §7: набор NEW SET сначала просит имя
    {Screen::ControlOptions, Screen::NewKeySetName, Rule::KeysetNewName, -1, 19, "0x48d191"},
    // 20. 10 -> 30/34  вход в набор: View для set 0, иначе редактор
    {Screen::ControlOptions, Screen::KeySetView, Rule::ItemEnter, 1, 20, "0x48d1b2"},
    // 21. 13 -> 23  New Single Player Game
    {Screen::StartGame, Screen::Difficulty, Rule::ItemEnter, 0, 21, "0x48da96"},
    // 22. 13 -> 19  New Multiplayer Game
    {Screen::StartGame, Screen::MultiplayerType, Rule::ItemEnter, 1, 22, "0x48dab8"},
    // 23. 14 -> 15  выбран слот -> ввод имени
    {Screen::SaveGame, Screen::SaveNameEntry, Rule::ItemEnter, 0, 23, "0x48e162"},
    // 24. 15 -> 14  Esc из ввода имени
    {Screen::SaveNameEntry, Screen::SaveGame, Rule::Escape, -1, 24, "0x48b710"},
    // 25. 16 -> 10  Esc из подэкрана
    {Screen::KeyJamming, Screen::ControlOptions, Rule::Escape, -1, 25, "0x48e599"},
    // 26. 17 -> 23  уровень выбран -> сложность
    {Screen::LevelSelect, Screen::Difficulty, Rule::ItemEnter, 0, 26, "0x48e281"},
    // 27. 18 -> 12  Esc из списка режимов
    {Screen::VideoMode, Screen::GraphicsOptions, Rule::Escape, -1, 27, "0x48e55a"},
    // 28. 21 -> 17  число игроков принято -> выбор уровня (пункт 0)
    {Screen::LocalPlayers, Screen::LevelSelect, Rule::ItemEnter, 0, 28, "0x48dca6"},
    // 29. 22 -> 10  Esc
    {Screen::MouseSetup, Screen::ControlOptions, Rule::Escape, -1, 29, "0x48e599"},
    // 30. 23 -> 24  ранг выбран -> имя игрока 1
    {Screen::Difficulty, Screen::PlayerOneName, Rule::ItemEnter, 0, 30, "0x48e3c7"},
    // 31. 26 -> 10  Esc
    {Screen::JoystickCentering, Screen::ControlOptions, Rule::Escape, -1, 31, "0x48e599"},
    // 32. 30 -> 10  Esc
    {Screen::KeySetView, Screen::ControlOptions, Rule::Escape, -1, 32, "0x48e599"},
    // 33. 34 -> 35  алиас: 34 -> 35 на следующем проходе ввода
    {Screen::KeySetEditorEntry, Screen::KeySetEditor, Rule::KeysetAlias, -1, 33, "0x48e212"},
    // 34. 34/35 -> 10  Esc (две исходные строки одного экрана)
    {Screen::KeySetEditorEntry, Screen::ControlOptions, Rule::Escape, -1, 34, "0x48e599"},
    {Screen::KeySetEditor, Screen::ControlOptions, Rule::Escape, -1, 34, "0x48e599"},
    // 35. 36 -> 10  Esc
    {Screen::JoystickSetup, Screen::ControlOptions, Rule::Escape, -1, 35, "0x48e599"},
};

static_assert(std::size(kTransitions) == 36,
              "§4: 35 table rows, row 34 stores both 34 and 35, so 36 entries");

/// §2 screens (ids ascending).  The four handler-less ids (7, 8, 28, 29) are
/// excluded on purpose: §1/§2 call them transitions, not screens.
constexpr std::array<Screen, 34> kScreens = {
    Screen::Intro,            Screen::Main,
    Screen::SmlPager,         Screen::BriefingTransitionA,
    Screen::BriefingTransitionB, Screen::Error,
    Screen::Restart,          Screen::InGameMenu,
    Screen::ControlOptions,   Screen::SoundOptions,
    Screen::GraphicsOptions,  Screen::StartGame,
    Screen::SaveGame,         Screen::SaveNameEntry,
    Screen::KeyJamming,       Screen::LevelSelect,
    Screen::VideoMode,        Screen::MultiplayerType,
    Screen::Shareware,        Screen::LocalPlayers,
    Screen::MouseSetup,       Screen::Difficulty,
    Screen::PlayerOneName,    Screen::PlayerTwoName,
    Screen::JoystickCentering, Screen::MultiplayerOptions,
    Screen::KeySetView,       Screen::NewKeySetName,
    Screen::PlayOptions,      Screen::Quit,
    Screen::KeySetEditorEntry, Screen::KeySetEditor,
    Screen::JoystickSetup,    Screen::NewVideoMode,
};

static_assert(std::size(kScreens) == 34, "§2: 34 screens after dropping 7/8/28/29");

// --- analysis-only edges --------------------------------------------------
//
// §4 does not contain the entry edges of the "new game"/"video mode" branches
// and of the SML pager, so a reachability walk over §4 alone stops at 18 of the
// 34 screens.  The edges below close the graph; every one is labelled with its
// evidence and none of them is applied by `feed()`.
constexpr EntryEdge kEntryEdges[] = {
    // §12/UI_SOUNDS §2.5: the title shows the help/credits pager; the §4
    // credits row (row 13) is struck out, so the exact site is unknown.
    {Screen::Main, Screen::SmlPager, "§12 + UI_SOUNDS §2.5 (вывод)"},
    // §1: driver state 2 drives the briefing transitions 3/4.
    {Screen::SmlPager, Screen::BriefingTransitionA, "§1 (вывод)"},
    {Screen::BriefingTransitionA, Screen::BriefingTransitionB, "§1 (вывод)"},
    // §3.3: item 0 of GRAPHICS OPTIONS is the video-mode line.
    {Screen::GraphicsOptions, Screen::VideoMode, "§3.3 item 0 (вывод)"},
    // §2 id 37 "NEW VIDEO MODE OK?" confirms a mode picked in id 18.
    {Screen::VideoMode, Screen::NewVideoMode, "§2 id 37 (вывод)"},
    // §4 prose: the chain 21 -> 17 -> 23 -> 24 is confirmed, the entry 19 -> 21
    // is register-based (esi, 0x48dce8) and therefore has no immediate.
    {Screen::MultiplayerType, Screen::LocalPlayers, "§4 проза (нет immediate, 0x48dce8)"},
    // §2 id 27 "MULTIPLAY GAME OPTIONS" follows the game type of id 19.
    {Screen::MultiplayerType, Screen::MultiplayerOptions, "§2 id 27 (вывод)"},
    // §2 id 20 is the shareware notice of the multiplayer branch (гипотеза).
    {Screen::MultiplayerType, Screen::Shareware, "§2 id 20 (гипотеза)"},
    // §2 id 25 is the same builder 0x489020(1) for player two, after 24.
    {Screen::PlayerOneName, Screen::PlayerTwoName, "§2 id 25 (вывод)"},
    // §4 row 20 has two targets; the editor branch is chosen by the set index.
    {Screen::ControlOptions, Screen::KeySetEditorEntry, "§4 строка 20, вторая ветка (§3.1)"},
    // §4 row 19 / §7: a set named NEW SET asks for a name first.
    {Screen::ControlOptions, Screen::NewKeySetName, "§4 строка 19 + §7"},
    // §4 row 33: the one-frame alias.
    {Screen::KeySetEditorEntry, Screen::KeySetEditor, "§4 строка 33 (0x48e212)"},
};

constexpr std::array<Screen, 3> kRoots = {
    Screen::Intro,      // §1: app start, 0x499fc0
    Screen::InGameMenu, // §4 row 11: Esc during play opens the in-game menu
    Screen::Error,      // §2 id 5: raised by the engine from any state
};

/// §7: at most 16 sets; the shipped `KEYCFG/` has PERFECT, SPLIT1, SPLIT2 and
/// `GUNMETAL.CFG` 0x1E0 names PERFECT/SPLIT1 for the two players.
constexpr std::array<KeySet, 16> shipped_key_sets() {
    std::array<KeySet, 16> sets{};
    sets[0] = KeySet{"PERFECT", true};
    sets[1] = KeySet{"SPLIT1", true};
    sets[2] = KeySet{"SPLIT2", true};
    for (std::size_t i = 3; i < sets.size(); ++i) {
        sets[i] = KeySet{"NEW SET", false};
    }
    return sets;
}

constexpr std::array<KeySet, 16> kShippedKeySets = shipped_key_sets();

const Transition* find_escape_row(Screen screen) {
    for (const Transition& row : kTransitions) {
        if (row.enabled && row.from == screen && row.rule == Rule::Escape) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

// --- FrontendMenu ---------------------------------------------------------

void FrontendMenu::reset() {
    key_set_ = {0, 1};
    key_sets_ = kShippedKeySets;
    set_screen(Screen::Main);
}

std::span<const Screen> FrontendMenu::screens() { return kScreens; }

std::span<const Transition> FrontendMenu::transitions() { return kTransitions; }

std::span<const EntryEdge> FrontendMenu::entry_edges() { return kEntryEdges; }

std::span<const Screen> FrontendMenu::roots() { return kRoots; }

std::string_view FrontendMenu::screen_name(Screen screen) {
    switch (screen) {
    case Screen::None: return "(none)";
    case Screen::Intro: return "INTRO VIDEO";
    case Screen::Main: return "MAIN MENU";
    case Screen::SmlPager: return "SML PAGER";
    case Screen::BriefingTransitionA: return "BRIEFING TRANSITION A";
    case Screen::BriefingTransitionB: return "BRIEFING TRANSITION B";
    case Screen::Error: return "ERROR";
    case Screen::Restart: return "RESTART GAME";
    case Screen::InGameMenu: return "IN-GAME MENU";
    case Screen::ControlOptions: return "CONTROL OPTIONS";
    case Screen::SoundOptions: return "SOUND OPTIONS";
    case Screen::GraphicsOptions: return "GRAPHICS OPTIONS";
    case Screen::StartGame: return "START GAME";
    case Screen::SaveGame: return "SAVE GAME";
    case Screen::SaveNameEntry: return "SAVE NAME ENTRY";
    case Screen::KeyJamming: return "KEY JAMMING";
    case Screen::LevelSelect: return "SELECT LEVEL FOR NEW GAME";
    case Screen::VideoMode: return "SELECT VIDEO MODE";
    case Screen::MultiplayerType: return "MULTIPLAYER GAME TYPE";
    case Screen::Shareware: return "LAY DOWN THE BUCKS";
    case Screen::LocalPlayers: return "NUMBER OF LOCAL PLAYERS";
    case Screen::MouseSetup: return "MOUSE SETUP";
    case Screen::Difficulty: return "SET DIFFICULTY LEVEL";
    case Screen::PlayerOneName: return "LOCAL PLAYER ONE NAME";
    case Screen::PlayerTwoName: return "LOCAL PLAYER TWO NAME";
    case Screen::JoystickCentering: return "JOYSTICK CENTERING";
    case Screen::MultiplayerOptions: return "MULTIPLAY GAME OPTIONS";
    case Screen::KeySetView: return "KEY SET VIEW";
    case Screen::NewKeySetName: return "NAME OF NEW KEY SET";
    case Screen::PlayOptions: return "PLAY OPTIONS";
    case Screen::Quit: return "QUIT GAME?";
    case Screen::KeySetEditorEntry: return "KEY SET EDITOR (ENTRY)";
    case Screen::KeySetEditor: return "KEY SET EDITOR";
    case Screen::JoystickSetup: return "PLAYER 1 JOYSTICK SETUP";
    case Screen::NewVideoMode: return "NEW VIDEO MODE OK?";
    case Screen::Exit: return "EXIT";
    }
    return "(unknown)";
}

int FrontendMenu::screen_id(Screen screen) {
    switch (screen) {
    case Screen::None:
    case Screen::Exit: return -1;
    default: return static_cast<int>(screen);
    }
}

bool FrontendMenu::enter(Screen screen) {
    if (screen == Screen::None) {
        return false;
    }
    set_screen(screen);
    return true;
}

const Transition* FrontendMenu::item_row(Screen screen, int item) const {
    for (const Transition& row : kTransitions) {
        if (row.enabled && row.from == screen && row.rule == Rule::ItemEnter &&
            row.item == item) {
            return &row;
        }
    }
    return nullptr;
}

void FrontendMenu::set_screen(Screen screen) {
    screen_ = screen;
    rebuild_items();
}

void FrontendMenu::rebuild_items() {
    const std::span<const Item> source = items_for(screen_);
    items_.assign(source.begin(), source.end());
    cursor_ = 0;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].selectable()) {
            cursor_ = i;
            return;
        }
    }
}

bool FrontendMenu::move_cursor(int delta) {
    if (items_.empty() || delta == 0) {
        return false;
    }
    // Model choice: the selection wraps around (no §4 row describes the ends of
    // a list).  Stops are the *first* row of every selectable item id: rows that
    // share an id (items 0/1 of `NUMBER OF LOCAL PLAYERS`, the two-line options
    // of `PLAY OPTIONS`) are one item, highlighted together (§3).
    std::vector<std::size_t> stops;
    stops.reserve(items_.size());
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].selectable()) {
            continue;
        }
        const bool seen = std::any_of(stops.begin(), stops.end(), [&](std::size_t j) {
            return items_[j].id == items_[i].id;
        });
        if (!seen) {
            stops.push_back(i);
        }
    }
    if (stops.empty()) {
        return false;
    }
    const std::size_t position = [&] {
        for (std::size_t i = 0; i < stops.size(); ++i) {
            if (stops[i] == cursor_) {
                return i;
            }
        }
        return std::size_t{0};
    }();
    const std::size_t step = static_cast<std::size_t>(delta < 0 ? -delta : delta);
    const std::size_t next =
        delta > 0 ? (position + step) % stops.size()
                  : (position + stops.size() - step % stops.size()) % stops.size();
    const std::size_t index = stops[next];
    if (index == cursor_) {
        return false;
    }
    cursor_ = index;
    return true;
}

bool FrontendMenu::adjust(int delta) {
    // The title carousel also rotates with Left/Right (model: the art entries
    // are a ring, §1 `0x4bc444--/++` is only documented for Up/Down).
    if (screen_ == Screen::Main) {
        return move_cursor(delta);
    }
    // §3.1: items 0/2 page through the selected key set (0..15), the only
    // Left/Right handling of this screen that has a byte-checked row.
    if (screen_ == Screen::ControlOptions && cursor_ < items_.size()) {
        const int id = items_[cursor_].id;
        if (id == 0 || id == 2) {
            int& slot = key_set_[id == 0 ? 0 : 1];
            slot = (slot + delta + 16) % 16;
            return true;
        }
    }
    return false;
}

bool FrontendMenu::open_key_set(int player) {
    const int slot = key_set_[player == 2 ? 1 : 0];
    if (!key_sets_[static_cast<std::size_t>(slot)].named) {
        // §7: a set called NEW SET first asks for a name (state 31).
        return enter(Screen::NewKeySetName);
    }
    if (slot == 0) {
        // §4 row 20: set 0 opens as a read-only view (`View %s`).
        return enter(Screen::KeySetView);
    }
    // §4 row 20: any other set opens the editor (34, aliased to 35).
    return enter(Screen::KeySetEditorEntry);
}

bool FrontendMenu::enter_pressed() {
    if (screen_ == Screen::Intro) {
        return enter(Screen::Main); // §4 row 1: intro finished or skipped
    }
    if (screen_ == Screen::Quit) {
        // §3.8: outside the game the screen asks `QUIT GAME?` with Yes/No.
        // `Exit` is the host's stop signal; `No` returns to the title.  §4 has
        // no row for either - this is the task-required quit path.
        if (cursor_ < items_.size()) {
            const int id = items_[cursor_].id;
            if (id == 0) {
                return enter(Screen::Exit);
            }
            if (id == 1) {
                return enter(Screen::Main);
            }
        }
        return false;
    }
    if (screen_ == Screen::ControlOptions) {
        // §3.1: items 1/3 enter the player's set (Assign/View), items 4..8 have
        // their own §4 rows and go through the table below.
        if (cursor_ < items_.size()) {
            const int id = items_[cursor_].id;
            if (id == 1) {
                return open_key_set(1);
            }
            if (id == 3) {
                return open_key_set(2);
            }
        }
    }
    if (screen_ == Screen::KeySetView || screen_ == Screen::KeySetEditor) {
        // Enter assigns/changes a binding in place (§3.7), not a screen change.
        return false;
    }
    if (cursor_ >= items_.size()) {
        return false;
    }
    const Transition* row = item_row(screen_, items_[cursor_].id);
    return row != nullptr && enter(row->to);
}

bool FrontendMenu::escape_pressed() {
    if (screen_ == Screen::Main) {
        // Task invariant: Esc on the title goes to QUIT.  §4 has no such row
        // (the original handles Esc only inside the sub-screens).
        return enter(Screen::Quit);
    }
    if (const Transition* row = find_escape_row(screen_)) {
        return enter(row->to);
    }
    return false;
}

bool FrontendMenu::feed(const KeyEvent& event) {
    if (screen_ == Screen::None || screen_ == Screen::Exit) {
        // No screen yet / the front-end has already stopped: safe no-op.
        return false;
    }

    bool changed = false;
    // §1: `0x48e212` rewrites 34 to 35 before the next input pass, so the alias
    // is consumed ahead of the key dispatch on the first event after entering.
    if (screen_ == Screen::KeySetEditorEntry) {
        set_screen(Screen::KeySetEditor);
        changed = true;
    }

    switch (event.kind) {
    case KeyEvent::Up: return move_cursor(-1) || changed;
    case KeyEvent::Down: return move_cursor(1) || changed;
    case KeyEvent::Left: return adjust(-1) || changed;
    case KeyEvent::Right: return adjust(1) || changed;
    case KeyEvent::Enter: return enter_pressed() || changed;
    case KeyEvent::Escape: return escape_pressed() || changed;
    // Backspace edits text (save name, §5.2), `Text`/`Other` carry key codes
    // this layer does not map: they never move the state.
    case KeyEvent::Backspace:
    case KeyEvent::Text:
    case KeyEvent::Other:
    default: return changed;
    }
}

Screen FrontendMenu::back() const {
    if (screen_ == Screen::Main) {
        return Screen::Quit; // task invariant, see escape_pressed()
    }
    if (screen_ == Screen::None || screen_ == Screen::Exit) {
        return screen_;
    }
    if (const Transition* row = find_escape_row(screen_)) {
        return row->to;
    }
    return screen_;
}

int FrontendMenu::key_set(int player) const {
    return key_set_[player == 2 ? 1 : 0];
}

int FrontendMenu::selected_id() const {
    return cursor_ < items_.size() ? items_[cursor_].id : -1;
}

bool FrontendMenu::key_set_unnamed(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kShippedKeySets.size())) {
        return false;
    }
    return !key_sets_[static_cast<std::size_t>(slot)].named;
}

} // namespace gm
