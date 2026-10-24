# Покрытие экранов фронтенда (C++)

Карта «экран → модуль → CLI → тест → числа кадра». Числа сняты командами из
третьей колонки на поставочной установке (`/mnt/d/games/Gunmetal 3dfx`,
v1.36); каждый модуль имеет свой CTest с теми же числами в
`PASS_REGULAR_EXPRESSION`. Карта экранов и переходов — `docs/UI_SCREENS.md`,
общая карта CLI — `docs/CLI.md`.

Все фронтенд-кадры пишутся в PNG одним и тем же способом (`--png OUT`,
320×200 или `--scale N`), поддерживают `--no-plate` (кадр без `PLATE`) и
`--fade-lut N` — точный слой `0x682f90` (уровень 11 = тождество, 12..15
различаются; `docs/HUD_LUT.md`, `docs/LUT_BUILDER.md`). Флаг `--fade-lut`
проверяется на **29 кадрах** (`cli_fade_lut`, `cli_fade_lut_built`).

## 1. Экраны со своими модулями

| Экран | Что это | Модуль | CLI | Тест | Ключевые числа |
|---:|---|---|---|---|---|
| 5 | `ERROR` | `ui/error_render.*` | `--error [--error-message TEXT]` | `error_render`, `cli_error_screen*` | `ERROR` y 14, слова с (10,34), `ESC TO RETURN TO MENU` y 91, ink 489 / 803 |
| 6 | `RESTART GAME` | `ui/restart_render.*` | `--restart [--restart-items N] [--restart-cursor N]` | `restart_render`, `cli_restart*` | 2..4 пункта, `x 10`, `y = 27+16i`, ink 585 / 898 / 1132 |
| 9 | in-game, меню нет | `ui/ingame_menu_render.*` | `--ingame-menu [--ingame-menu-bg DEMOTION\|DISBAND]` | `ingame_menu_render`, `cli_ingame_menu*` | полноэкранный `DEMOTION`/`DISBAND` 320×200, ink 0; `DEMOTION` vs `DISBAND` — 49 323/64 000 px |
| 10 | `CONTROL OPTIONS` | `ui/control_options_render.*` | `--control-options [--control-options-cursor N] [--control-options-keyset1 N] [--control-options-keyset2 N] [--control-options-joystick1 N] [--control-options-joystick2 N] [--control-options-mouse N]` | `control_options_render`, `cli_control_options*` | 10 строк `y = 27+9i`, `x 20/32`, ink 1736 |
| 11 | `SOUND OPTIONS` | `ui/sound_options_render.*` | `--sound-options [--sound-options-cursor N] [--sound-options-music N] [--sound-options-effects N] [--sound-options-swap] [--sound-options-off] [--sound-options-low-memory]` | `sound_options_render`, `cli_sound_options*` | 7 строк `y = 27+10i`, 2 слайдера, 1 чекбокс, ink 1869; off — `rows=1 ink=1098` |
| 13 | `START GAME` + слоты | `ui/start_game_render.*` | `--start-game` | `start_game_render`, `cli_start_game*` | 20 слотов, `FILESLOT` (35, 43+11i), occupied 2 |
| 14 | `SAVE GAME` / `LOAD GAME` | `ui/save_menu_render.*` | `--save-menu` | `save_menu_render`, `cli_save_menu*` | 20 слотов, `FILESLOT` (38, 27+12i), стрелки `MENUARRW` |
| 15 | ввод имени сейва | `ui/save_name_render.*` | `--save-name [--save-name-text NAME] [--save-name-row N] [--save-name-no-caret] [--no-fileslot]` | `save_name_render`, `cli_save_name*` | `SAVE GAME` y 14, `ESC TO CANCEL` y 101, имя (42, 28+12·row) + каретка `'-'`, буфер 17 симв., ink 544 |
| 16 | `KEY JAMMING` | `ui/key_jamming_render.*` | `--key-jamming [--key-jamming-keys 30,31] [--key-jamming-cursor N]` | `key_jamming_render`, `cli_key_jamming*` | 4 строки (14/26/35/44), имена клавиш `x = 30+34·(n%4)`, `y = 54+8·(n/4)`, ink 752 / 939 |
| 17 | `SELECT LEVEL` | `ui/level_select_render.*` | `--level-select [--level-select-counter N] [--level-select-row N]` | `level_select_render`, `cli_level_select*` | 36 уровней, окно 10, счётчик 1 — 7 открытых |
| 18 | `SELECT VIDEO MODE` | `ui/video_mode_list_render.*` | `--video-mode-list [--video-mode-list-cursor N]` | `video_mode_list_render`, `cli_video_mode_list*` | 15 режимов (модель), окно 10, `y = 29+9i`, стрелки 41 px, ink 2838 |
| 19 | тип игры | `ui/game_type_render.*` | `--game-type [--game-type-mask HEX]` | `game_type_render`, `cli_game_type*` | mask `0xff` — 12 строк, 11 режимов, `cancel_row 106`, ink 2478 |
| 20 | `LAY DOWN THE BUCKS` | `ui/lay_bucks_render.*` | `--lay-bucks` | `lay_bucks_render`, `cli_lay_bucks*` | 4 строки (14/27/36/45), ни суммы, ни курсора, ink 1036 |
| 21 | число локальных игроков | `ui/local_players_render.*` | `--local-players [--local-players-cursor N]` | `local_players_render`, `cli_local_players*` | 3 пункта по 2 строки (27/36), шаг 27, ink 936 |
| 22 | `MOUSE SETUP` | `ui/mouse_setup_render.*` | `--mouse-setup [--mouse-setup-cursor N]` | `mouse_setup_render`, `cli_mouse_setup*` | 12 строк `y = 24+8i`, 3 слайдера `SLIDE`/`KNOB`, кноб `108 − trunc((v−0.2)·(−27))`, ink 3986 |
| 23 | сложность | `ui/difficulty_render.*` | `--difficulty [--difficulty-cursor N]` | `difficulty_render`, `cli_difficulty*` | 6 строк `y = 27+15i`, ink 768 |
| 24/25 | имена локальных игроков | `ui/player_name_render.*` | `--player-name [--player-name-player N] [--player-name-text NAME] [--player-name-caret-pos N] [--player-name-no-caret]` | `player_name_render`, `cli_player_name*` | заголовки 24/25 y 14, имя `FONTS2` y 28 по центру, каретка 8×17, лимит 10, ink 1812/2331 |
| 26 | `JOYSTICK CENTERING` | `ui/joystick_centering_render.*` | `--joystick-centering [--joystick-centering-cursor N]` | `joystick_centering_render`, `cli_joystick_centering*` | 5 статических строк (25/35/45, 86/97), x 36/41, ink 1141 |
| 27 | `MULTIPLAY GAME OPTIONS` | `ui/match_setup_render.*` | `--match-setup [--match-setup-cursor N] [--match-setup-type N] [--match-setup-cash N] [--match-setup-flags HEX]` | `match_setup_render`, `cli_match_setup*` | 8 пунктов, 5 чекбоксов по флагам `0x75`, ink 2239 / 2362 |
| 30, 34–35 | редактор клавиш | `ui/keycfg_render.*` | `--keycfg NAME [--keycfg-cursor N] [--keycfg-mode view\|edit] [--keycfg-cell N] [--keycfg-conflict]` | `keycfg_render`, `cli_keycfg*` | 31 строка, `PERFECT.key` — 29 имён, ink 2659 |
| 36 | `PLAYER 1/2 JOYSTICK SETUP` | `ui/player_setup_render.*` | `--player-setup [--player-setup-cursor N] [--player-setup-joystick N] [--player-setup-player N]` | `player_setup_render`, `cli_player_setup*` | 1 строка без устройства / 43 с ним, `x 13`, `y = 28+9i`, окно 10, ink 825 / 2640 |
| 31 | `NAME OF NEW KEY SET` | `ui/new_keyset_render.*` | `--new-keyset [--new-keyset-text NAME] [--new-keyset-no-caret]` | `new_keyset_render`, `cli_new_keyset*` | заголовок y 14, имя 16×16 y 28, каретка-рамка 8×17, лимит 15, ink 1849 |
| 33 | `QUIT GAME?` / `QUIT OPTIONS` | `ui/quit_render.*` | `--quit [--quit-in-game] [--quit-cursor N]` | `quit_render`, `cli_quit*` | 2 строки ink 228 / 3 строки ink 576 (y 27/43/59) |
| 37 | `NEW VIDEO MODE OK?` | `ui/video_mode_render.*` | `--video-mode [--video-mode-cursor N]` | `video_mode_render`, `cli_video_mode*` | `Yes`/`No` y 27/43, ink 356 |

Общие кадры без отдельного модуля:

* `--menu` — экраны меню с пунктами (`ui/menu_render.*`, `docs/MENU_RENDER.md`),
  включая слайдеры/чекбоксы;
* `--pager NAME` — текстовый пагер `.sme` (`ui/pager_render.*`);
* `--briefing LEVEL` — брифинг миссии (`ui/briefing_render.*`);
* `--hud-png`, `--overlay-png` — HUD и оверлеи (`render/hud_overlay.*`).

## 2. Ещё не покрыто

* Все содержательные экраны v1.36 (5–37) теперь имеют модуль, CLI и тест;
  общие кадры меню/пагера/брифинга — через `ui/menu_render.*`,
  `ui/pager_render.*`, `ui/briefing_render.*`.
* Экраны, которые вообще не выводят кадр (переходы, загрузка), — вне карты.

## 3. Как воспроизвести числа

```bash
E=build/sandbox/engine/gunmetal; G="/mnt/d/games/Gunmetal 3dfx"
for f in --restart --lay-bucks --save-name --match-setup --key-jamming; do
  $E --game "$G" $f | head -1
done
$E --game "$G" --save-name --save-name-row 3 \
   --save-name-text "HELLO WORLD THIS IS LONG" | head -1
ctest --test-dir build/sandbox -R '^cli_(fade_lut|save_name|lay_bucks)$'
```

Счётчик опций CLI сверяет `tools/qa/doc_numbers.py --check --strict`
(сейчас 207); общее состояние — `python3 tools/qa/state.py --check`.
