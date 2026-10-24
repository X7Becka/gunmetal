# CLI движка: карта режимов и PNG-кадров

`gunmetal --help` — единственный авторитетный список: он собирается из
`engine/src/core/config.cpp` (`usage()`), и **все 199 опции** парсера в нём
есть (проверено сравнением `grep '"--'` по парсеру с выводом `--help`,
2026-10-24; до этого 11 опций `--hud*`, `--lm-world` и `--spawn-*` в
подсказке отсутствовали).

Общие правила:

* `-g, --game DIR` — корень установки; по умолчанию `$GUNMETAL_DIR` или
  `/mnt/d/games/Gunmetal 3dfx`. Каталог обязан содержать `gundx.exe`.
* Код возврата `0` — успех, `2` — отказ (текст в stderr с префиксом
  `[error] <модуль>:`). PNG и правки пишутся только по явному пути.
* Установка всегда **только на чтение**: `--save-out`, `--save-action-out`
  и инструменты `tools/editor/*` отказываются писать в игру.

## 1. Фронтенд: PNG-кадры экранов

Все кадры — 320×200 `0xRRGGBBAA`, целочисленный зум `--png-scale N`
(1..8), `--no-plate` убирает плату `PLATE` (чёрный холст).

| режим | что рисует | кадр поставки | CTest |
|---|---|---|---|
| `--menu --png OUT [--menu-screen ID] [--fade-lut N]` | текущий экран фронтенда (§2 `UI_SCREENS`) | панель из `BMP.IDX` | `cli_menu_png`, `cli_fade_lut` |
| `--pager FILE --png OUT` | SML-страницу (`.sml`/`.sme`); `--fade-lut N` — точные таблицы `0x682f90` (11 = тождество, 12..15 различаются; дамп или сборка из `GAMEPAL`) | `HELPSCRN` по `--background` | `cli_pager_*`, `cli_fade_lut`, `cli_fade_lut_built` |
| `--briefing LEVEL --png OUT [--page N] [--fade-lut N]` | страницу брифинга через пагер | `level1` стр. 1: 15 строк / 719 глифов / ink 7214 | `cli_briefing_png*` |
| `--level-select --png OUT [--level-select-counter N] [--level-select-row N] [--fade-lut N]` | экран 17 | 36 строк / 7 открыто, ink 1305, плата 196×131 | `cli_level_select*` |
| `--save-menu --png OUT [--save-menu-slot N] [--save-menu-mode load\|save] [--fade-lut N]` | экран 14 | 20 слотов / 2 занято, ink 917 | `cli_save_menu*` |
| `--start-game --png OUT [--save-menu-slot N] [--fade-lut N]` | экран 13 | 8 строк / 6 слотов, ink 1608, 1 стрелка | `cli_start_game*` |
| `--game-type --png OUT [--game-type-mask HEX] [--game-type-cursor N] [--fade-lut N]` | экран 19 (тип мультиплеера, маска `0x4c1290`) | 11 режимов + `Cancel`, `y=18+8k`, ink 2478 | `cli_game_type*`, `cli_fade_lut` |
| `--difficulty --png OUT [--difficulty-cursor N] [--fade-lut N]` | экран 23 (сложность) | 6 строк, `y=27+15i`, ink 768 | `cli_difficulty*`, `cli_fade_lut` |
| `--local-players --png OUT [--local-players-cursor N] [--fade-lut N]` | экран 21 (число игроков) | 3 пункта по 2 строки (27/36), ink 936 | `cli_local_players*`, `cli_fade_lut` |
| `--quit --png OUT [--quit-in-game] [--quit-cursor N] [--fade-lut N]` | экран 33 (`QUIT GAME?`/`QUIT OPTIONS`) | 2 строки ink 228 / 3 строки ink 576 | `cli_quit*`, `cli_fade_lut` |
| `--video-mode --png OUT [--video-mode-cursor N] [--fade-lut N]` | экран 37 (`NEW VIDEO MODE OK?`) | `Yes`/`No` (27/43), ink 356 | `cli_video_mode*`, `cli_fade_lut` |
| `--restart --png OUT [--restart-items N] [--restart-cursor N] [--fade-lut N]` | экран 6 (`RESTART GAME`) | 2..4 пункта (x 10, y 27+16i), ink 585/898/1132 | `cli_restart*`, `cli_fade_lut` |
| `--video-mode-list --png OUT [--video-mode-list-cursor N] [--fade-lut N]` | экран 18 (список режимов) | 15 строк (модель), окно 10, `y=29+9i`, ink 2838 | `cli_video_mode_list*`, `cli_fade_lut` |
| `--mouse-setup --png OUT [--mouse-setup-cursor N] [--fade-lut N]` | экран 22 (`MOUSE SETUP`) | 12 строк, 3 слайдера `SLIDE`/`KNOB`, ink 3986 | `cli_mouse_setup*`, `cli_fade_lut` |
| `--joystick-centering --png OUT [--joystick-centering-cursor N] [--fade-lut N]` | экран 26 (`JOYSTICK CENTERING`) | 5 строк (25/35/45, 86/97), ink 1141 | `cli_joystick_centering*`, `cli_fade_lut` |
| `--match-setup --png OUT [--match-setup-cursor N] [--match-setup-type N] [--match-setup-cash N] [--match-setup-flags HEX] [--fade-lut N]` | экран 27 (`MULTIPLAY GAME OPTIONS`) | 8 пунктов, 5 чекбоксов `CHECK`/`NOCHECK`, ink 2239/2362 | `cli_match_setup*`, `cli_fade_lut` |
| `--key-jamming --png OUT [--key-jamming-keys 30,31] [--key-jamming-cursor N] [--fade-lut N]` | экран 16 (`KEY JAMMING`) | 4 строки + до 28 имён клавиш, ink 752/939 | `cli_key_jamming*`, `cli_fade_lut` |
| `--ingame-menu --png OUT [--ingame-menu-bg DEMOTION\|DISBAND] [--fade-lut N]` | экран 9 (in-game, меню нет) | полноэкранный `DEMOTION`/`DISBAND` 320×200 | `cli_ingame_menu*`, `cli_fade_lut` |
| `--hud-lut-dump OUT` | LUT `0x682f90`, построенный из `GAMEPAL` | 4096 Б, FNV `0x6330406254771f3d`; внутри установки писать отказывается | `cli_hud_lut_dump*` |
| `--error --png OUT [--error-message TEXT] [--fade-lut N]` | экран 5 (`ERROR`) | заголовок y 14, слова с (10,34), `ESC TO RETURN TO MENU` y 91, ink 489 | `cli_error_screen*`, `cli_fade_lut` |
| `--player-setup --png OUT [--player-setup-cursor N] [--player-setup-joystick N] [--player-setup-player N] [--fade-lut N]` | экран 36 (`PLAYER 1/2 JOYSTICK SETUP`) | 1 строка без устройства / 43 с устройством, `x 13`, окно 10, ink 825/2640 | `cli_player_setup*`, `cli_fade_lut` |
| `--control-options --png OUT [--control-options-cursor N] [--control-options-keyset1 N] [--control-options-keyset2 N] [--control-options-joystick1 N] [--control-options-joystick2 N] [--control-options-mouse N] [--fade-lut N]` | экран 10 (`CONTROL OPTIONS`) | 10 строк `y = 27+9i`, ink 1736 | `cli_control_options*`, `cli_fade_lut` |
| `--sound-options --png OUT [--sound-options-cursor N] [--sound-options-music N] [--sound-options-effects N] [--sound-options-swap] [--sound-options-off] [--sound-options-low-memory] [--fade-lut N]` | экран 11 (`SOUND OPTIONS`) | 7 строк `y = 27+10i`, 2 слайдера, 1 чекбокс, ink 1869 (off — 1098) | `cli_sound_options*`, `cli_fade_lut` |
| `--graphics-options --png OUT [--graphics-options-cursor N] [--graphics-options-border N] [--graphics-options-gamma N] [--graphics-options-reflections N] [--graphics-options-light-maps N] [--fade-lut N]` | экран 12 (`GRAPHICS OPTIONS`) | 6 строк (24/39/54/69/84/98), слайдер `Border`, чекбокс `Reflections`, ink 1866 | `cli_graphics_options*`, `cli_fade_lut` |
| `--play-options --png OUT [--play-options-cursor N] [--play-options-flags HEX] [--play-options-camera N] [--play-options-in-game] [--fade-lut N]` | экран 32 (`PLAY OPTIONS`) | 8 пунктов, 6 чекбоксов, ink 2765 (in-game — 2854) | `cli_play_options*`, `cli_fade_lut` |
| `--lay-bucks --png OUT [--fade-lut N]` | экран 20 (`LAY DOWN THE BUCKS`, shareware-заглушка) | 4 строки (14/27/36/45), ink 1036 | `cli_lay_bucks*`, `cli_fade_lut` |
| `--save-name --png OUT [--save-name-text NAME] [--save-name-row N] [--save-name-no-caret] [--no-fileslot] [--fade-lut N]` | экран 15 (ввод имени сейва) | `SAVE GAME` y 14, `ESC TO CANCEL` y 101, имя + `FILESLOT`, ink 544 | `cli_save_name*`, `cli_fade_lut` |
| `--keycfg NAME --png OUT [--keycfg-cursor N] [--keycfg-mode view\|edit] [--keycfg-cell a\|b] [--keycfg-conflict TEXT] [--fade-lut N]` | экран 30 / редактор 34–35 (`KEYCFG/NAME.key`) | `PERFECT`: 31 строка / 29 имён / ink 2659 | `cli_keycfg*` |
| `--hud-png OUT` | кабина (панель/иконки/числа) | см. `HUD_CPP.md` | `cli_hud_png*` |
| `--render OUT` | мир (уровень или `--model`) | см. `RENDER_CPP.md` | `render_*` |

Флаги кадра: `--fade K` (0..15, модель пагера), `--fade-lut K` (точные
таблицы `0x682f90` на палитровых индексах; 11 = без fade), `--background`
(`HELPSCRN` за страницей), `--page N` (1-based), `--hud-weapon/--hud-ammo/
--hud-armor/--hud-kills/--hud-speed` для `--hud-png`,
`--hud/--hud-screen/--hud-tick/--hud-x/--hud-y/--hud-scale/--hud-opacity`
для наложения HUD на `--render`.

## 2. Данные и текст

| опция | что печатает |
|---|---|
| `-d, --dump FILE` | размер и первые байты; для `.sbm`/`.bmp` — ещё и PNG по `--png` |
| `-l, --level NAME` | сводку уровня (с `--info`) |
| `--film NAME [--frame N]` | тайлы `.sof`, `.fdx`, FNV кадра |
| `--anim NAME [--at MS]` | позу `.soo` (в `--render`) |
| `--playlist FILE` | резолв `MUSIC/*.sog` в `MUSIC/SOU.IDX` (`entries/found/missing/bytes`) |
| `--best-times` | строки `GUNMETAL.HI` и минимум по уровню |
| `--things-anim [--level NAME] [--things-anim-tick N] [--things-anim-limit N]` | привязка things к клипу/кадру (`collect` + первые N строк) |
| `--killed NAME [--killed-all] [--killed-seed N]` | строки KILLED с подстановкой `%S` |
| `--save-slots` | список слотов `SAVEGAME` |
| `--save-load SLOT [--save-load-ticks N]` | загрузка сейва в одноразовый рантайм |
| `--save-out OUT`, `--set K=V` | запись правленого сейва (только вне игры) |

## 3. Сейв-действия (срезы `M8.7`)

```
gunmetal --save-action load --save-menu-slot 0
  → result=loaded, level=level0, feet=(1116.173752,798.207621,48.0),
    blocks=15, sector_stuff=13

gunmetal --save-action save --save-menu-slot 0 --save-action-out /tmp/x
  → result=saved, 243368 Б, sha256 e39c0c35…
```

Пустой слот — `refused_empty` (файл не создаётся), запись в установку —
`refused_write`.

## 4. Симуляция и отладка

`--sim`, `--sim-players/--sim-mode/--sim-difficulty/--sim-drive/
--sim-projectile/--sim-vitals/--sim-respawn/--god`, `--spawn-camera/
--spawn-slot/--spawn-eye` (старт камеры у кампанийной точки), `--headless`,
`--third-person/--third-distance/--third-height` — подробности в
`SIM_CPP.md`, `SPAWN.md`, `CAMERA.md`.

## 5. Музыка и звук

`--music`, `--sou1/--sou2`, `--fade-in SEC`, `--fade-out SEC` — см.
`MUSIC.md`, `SOUND.md`.

## 6. Рендер мира

`--render OUT` + `--width/--height/--az/--el/--zoom/--fov`,
`--no-textures/--no-lightmaps/--lm-world/--wire/--mip/--filter/--uv-texel/
--alpha-test/--transparent-index/--chromakey/--haze/--skip-untextured`,
`--things/--thing-scale` — см. `RENDER_CPP.md`, `TEXUV.md`, `ALPHA.md`,
`SCREEN_FX.md`.

## 7. Проверки

`--help` покрыт CTest `cli_help` (наличие ключевых опций); таблица
фронтенд-кадров выше сверена с реальными прогонами (числа — из
модульных тестов этих экранов, `docs/*_RENDER.md`).
