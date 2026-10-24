# Индекс документации

Сгенерировано `python3 tools/ctx.py index`; не редактировать вручную.
Читать целиком большие файлы не нужно: `ctx.py doc <файл> <раздел>`,
`ctx.py addr 0x...`, `ctx.py outline <файл>`. Полный список — `ctx.py ls`.

| файл | КБ | адресов | о чём |
|---|---:|---:|---|
| `docs/ADR/0001-language.md` | 2 | 0 | ADR 0001 — язык и стек целевого движка |
| `docs/ADR/0002-scope.md` | 2 | 0 | ADR 0002 — что считается «готовым движком» |
| `docs/ADR/0003-saves.md` | 2 | 0 | ADR 0003 — сохранения: читать оригинал, писать совместимо где возможно |
| `docs/ADR/0004-re-infrastructure.md` | 3 | 0 | ADR 0004 — инфраструктура реверса: кэш, зеркало, бюджет контекста |
| `docs/AI_CPP.md` | 79 | 76 | Gunmetal — минимальный C++ AI вещей (`Ai`) |
| `docs/ALPHA.md` | 17 | 74 | Альфа мировых текстур и постоянный цвет кадра (M4.1.5) |
| `docs/AUDIO_3D.md` | 44 | 316 | 3D-позиционный звук (M7.3): очередь, слушатель, формулы |
| `docs/AUDIO_MIXER.md` | 14 | 8 | `audio/mixer`: детерминированный software-микшер |
| `docs/AUDIO_OUTPUT.md` | 16 | 0 | `platform/audio`: вывод звука (SDL3 + null) |
| `docs/AUDIT_NUMBERS.md` | 28 | 66 | AUDIT_NUMBERS — независимая сверка чисел в свежих RE-доках |
| `docs/BASELINE.md` | 12 | 0 | Baseline: установка, инвентарь и регрессия |
| `docs/BIGFILE_WRITE.md` | 17 | 46 | BIGFILE_WRITE — запись архивов `.IDX`/`.DAT` (`M11.6`) |
| `docs/BMP_WRITE.md` | 25 | 1 | Запись архива картинок `FRONTEND/BMP.IDX` (`M11.5`, срез 10) |
| `docs/BRIEFING.md` | 21 | 6 | Брифинги миссий: `SML/brief/*.SME` -> уровень, страницы, строки |
| `docs/BRIEFING_RENDER.md` | 27 | 10 | PNG-кадр брифинга: `SML/brief/<level>.SME` -> 320×200 RGBA |
| `docs/BUILD.md` | 4 | 0 | Сборка движка (M3) |
| `docs/CAMERA.md` | 31 | 209 | Gunmetal — камера первого лица: оси, матрица, проекция |
| `docs/CAMERA_CPP.md` | 25 | 74 | `camera/camera.h` — камера первого/третьего лица |
| `docs/CAMERA_MODEL.md` | 19 | 27 | `render/camera_model.h` — режимы камеры, focal adjust, детерминированная тряска |
| `docs/CAMPAIGN_FLOW.md` | 35 | 302 | Поток кампании: счётчик миссий, штаб, цели, секреты, время, финал (`M9.1`) |
| `docs/CAMPAIGN_STATE.md` | 15 | 59 | Прогресс кампании в движке (`M9.1`, срез 2) |
| `docs/CHASSIS.md` | 22 | 236 | Шасси: выбор, 57 CHEVY, модели/звуки, HUD (`M6.1.4`, `M6.5.3`) |
| `docs/CHASSIS_PHYSICS.md` | 13 | 16 | Физика шасси (`M6.1.1`): масса, скорость, drag, подвеска |
| `docs/CLI.md` | 12 | 4 | CLI движка: карта режимов и PNG-кадров |
| `docs/COCKPIT.md` | 22 | 40 | Кокпит: собранный кадр 320×200 (M4.4.5, срез 3) |
| `docs/COLLIDE.md` | 32 | 9 | Gunmetal — примитивы коллизий `.sbw` |
| `docs/COLLIDE_GRID.md` | 22 | 0 | Gunmetal — broad-phase сетка коллизий (`CollideGrid`) |
| `docs/COLLIDE_OVERLAP.md` | 24 | 0 | Gunmetal — объёмные запросы к статике (`collide_overlap`) |
| `docs/CONFIG.md` | 28 | 241 | `GUNMETAL.CFG` / `CONTROLS.CFG` — карта полей |
| `docs/CONTROLS_CPP.md` | 13 | 19 | `input/controls.h` — `.key` profiles and controller config |
| `docs/CONTROL_OPTIONS_RENDER.md` | 17 | 137 | Рендер экрана 10 `CONTROL OPTIONS` (`engine/src/ui/control_options_render.h`) |
| `docs/DAMAGE.md` | 31 | 255 | Урон, броня и щиты (`M6.5.2`) |
| `docs/DIFFICULTY_RENDER.md` | 14 | 67 | Рендер экрана 23 `SET DIFFICULTY LEVEL` (`engine/src/ui/difficulty_render.h`) |
| `docs/DM_RULES.md` | 31 | 394 | Deathmatch: режимы, счёт, respawn, гандикап, команды (`M9.2`) |
| `docs/DM_STATE.md` | 10 | 37 | Deathmatch state in the engine (`M9.2`, slice 2) |
| `docs/DOORS_CPP.md` | 55 | 137 | Gunmetal — C++-модель дверей, переключателей и лифтов |
| `docs/DOOR_COLLISION.md` | 29 | 6 | Gunmetal — двери и лифты в коллизии рантайма |
| `docs/DOOR_FIELDS.md` | 56 | 566 | Поля дверей, переключателей и лифтов (M2.3.4) |
| `docs/ECONOMY.md` | 27 | 386 | ECONOMY — деньги, магазины, масса (`M6.2.6`) |
| `docs/ECONOMY_STATE.md` | 12 | 31 | ECONOMY_STATE — деньги и магазин в движке (`M6.2.6`, срез 2) |
| `docs/EFFECTS.md` | 38 | 199 | Отрицательные индексы текстур: кадры спрайтов/анимаций в кадровом проходе (M4.2.6) |
| `docs/ENTITY_FIELDS.md` | 37 | 262 | Скриптовая таблица сущностей (`0x5b0df0`, `0x5e4680`, `0x616658`): карта (M2.3.6) |
| `docs/EQUIPMENT.md` | 30 | 318 | EQUIPMENT — модель экипировки (`DEFINE_EQUIPMENT`, состояние, эффекты) |
| `docs/EQUIPMENT_ACT.md` | 15 | 23 | EQUIPMENT_ACT — активация и расход оборудования (`M6.2.4`, срез 2) |
| `docs/EQUIPMENT_STATE.md` | 12 | 23 | EQUIPMENT_STATE — состояние оборудования в рантайме (`M6.2.4`, срез 2) |
| `docs/ERROR_RENDER.md` | 21 | 106 | Рендер экрана 5 `ERROR` (`engine/src/ui/error_render.h`) |
| `docs/EVENTS.md` | 24 | 92 | Event queue and `GENERATE` events |
| `docs/EVENT_DAMAGE.md` | 13 | 19 | Событийный урон по вещам: `damaged_event` → `GENERATE` → blast |
| `docs/FAQ.md` | 14 | 0 | FAQ — частые вопросы |
| `docs/FILM.md` | 27 | 107 | HUD-фильмы `.sof` / `.fdx` — формат, привязка, паритет |
| `docs/FIXTURES.md` | 28 | 5 | FIXTURES — синтетические минимальные тест-векторы (`M3.2.7`) |
| `docs/FOG.md` | 23 | 220 | Туман по глубине: формула, таблица `lighttable`, Glide (M4.1.6) |
| `docs/FOG_CPP.md` | 18 | 31 | Туман по глубине в C++ (`M4.1.6`, срез 2) |
| `docs/FORMATS.md` | 204 | 952 | Gunmetal — спецификация форматов данных |
| `docs/FRAME_METRIC.md` | 23 | 0 | Метрика сравнения кадров (M4.4.3) |
| `docs/FRONTEND.md` | 20 | 83 | Безголовый автомат меню (`M8`, срез 1) |
| `docs/FUZZ_CORPUS.md` | 11 | 1 | Фазз-корпус: сохранение и повтор (save / replay) |
| `docs/FUZZ_CPP.md` | 6 | 3 | C++ reader fuzzing (`engine/tests/test_fuzz.cpp`) |
| `docs/FUZZ_SAVES.md` | 13 | 1 | Фаззинг сейвов и миров через CLI (`tools/qa/fuzz_saves.py`, M12.1 slice 2) |
| `docs/FX_FRAME.md` | 15 | 20 | Резолв кадров эффектов (M4.2.6, срез 2) |
| `docs/GAME_CFG.md` | 13 | 51 | `GUNMETAL.CFG` в движке (`M12.5`/`M1.5`, срез 1) |
| `docs/GAME_FLOW.md` | 16 | 42 | Death, respawn and "game over" in the engine (`M9.5`, slice 1) |
| `docs/GAME_OVER.md` | 28 | 318 | Game over, титры, рестарт и экран конца уровня (`M9.5`) |
| `docs/GAME_STATE.md` | 15 | 14 | Gunmetal — C++-слой игрового состояния (`GameState`) |
| `docs/GAME_TYPE_RENDER.md` | 16 | 118 | Рендер экрана 19 `MULTIPLAYER GAME TYPE` (`engine/src/ui/game_type_render.h`) |
| `docs/GHIDRA.md` | 22 | 37 | Ghidra: выбор, установка и рабочий процесс |
| `docs/GRAPHICS_OPTIONS_RENDER.md` | 20 | 124 | Рендер экрана 12 `GRAPHICS OPTIONS` (`engine/src/ui/graphics_options_render.h`) |
| `docs/HI_SCORES.md` | 14 | 40 | Экран лучших времён (`GUNMETAL.HI`) — `M8.4`, срез 2 |
| `docs/HI_WRITE.md` | 26 | 39 | Запись `GUNMETAL.HI`: побайтовый round-trip и правка одной записи (`M11.5`, срез 11) |
| `docs/HUD_AUDIT.md` | 49 | 160 | HUD кокпита `level0`: инвентарь элементов (M4.4) |
| `docs/HUD_BIND.md` | 17 | 29 | HUD binding: `.sml`/`.sme` экраны → семейства `.sof`/`.fdx` и позиции `<I>` |
| `docs/HUD_COMPOSE.md` | 34 | 43 | HUD composition: `.sml` + `.sof/.fdx` + `blit_indexed` за один вызов |
| `docs/HUD_CPP.md` | 8 | 2 | C++ HUD-слой: блит кадров `.sof` |
| `docs/HUD_ICONS.md` | 27 | 210 | HUD: иконки 24×24, компас и источники чисел (M4.4.5, продолжение) |
| `docs/HUD_ICONS_CPP.md` | 14 | 19 | HUD-иконки и компас: C++ (`id -> FRONTEND/BMP.IDX`, геометрия ленты) |
| `docs/HUD_LUT.md` | 22 | 18 | HUD fade LUT: точные таблицы `0x682f90` (`M8.5`, срез 7) |
| `docs/HUD_OVERLAY.md` | 16 | 0 | HUD overlay: `hud_overlay.*` (M8.6, срез 2) |
| `docs/HUD_PLAYER.md` | 14 | 13 | C++ HUD-фильмы: проигрывание `.sof` + `.fdx` во времени |
| `docs/INGAME_MENU_RENDER.md` | 18 | 120 | Рендер экрана 9, in-game (кадр `DEMOTION`/`DISBAND`) |
| `docs/INIT.md` | 32 | 556 | Инициализация движка: WinMain → видео → звук → ввод → конфиг → палитра (M2.2.1) |
| `docs/INSTALL_AUDIT.md` | 12 | 0 | Аудит установки по манифесту |
| `docs/JOURNAL.md` | 148 | 320 | JOURNAL — журнал ходов |
| `docs/JOYSTICK_CENTERING_RENDER.md` | 17 | 102 | Рендер экрана 26 `JOYSTICK CENTERING` (`engine/src/ui/joystick_centering_render.h`) |
| `docs/KEYCFG_RENDER.md` | 37 | 213 | Рендер экрана 30 и редактора 34/35 — набор клавиш (`engine/src/ui/keycfg_render.h`) |
| `docs/KEY_JAMMING_RENDER.md` | 18 | 103 | Рендер экрана 16 `KEY JAMMING` (`engine/src/ui/key_jamming_render.h`) |
| `docs/KEY_WRITE.md` | 16 | 12 | Запись `.key` (KEYCFG): побайтовый round-trip и правка одного слота (`M11.5`, срез 13) |
| `docs/KILLED.md` | 12 | 16 | Экран сообщений об убийстве (`KILLED.skb` / `.skk`) — `M8.4`, срез 3 |
| `docs/KILLED_WRITE.md` | 24 | 7 | Запись таблиц убийств `KILLED.skb` / `.skk` (`M11.5`, срез 12) |
| `docs/LAYOUT.md` | 8 | 0 | LAYOUT — каталоги, конфигурация, канон |
| `docs/LAY_BUCKS_RENDER.md` | 17 | 94 | Рендер экрана 20 `LAY DOWN THE BUCKS` (`engine/src/ui/lay_bucks_render.h`) |
| `docs/LEVEL_LOAD.md` | 32 | 672 | Загрузка и выгрузка уровня: `.sbw`, `.sbt`, `.sbc`, `SOB_TABLE` (M2.2.3) |
| `docs/LEVEL_SELECT.md` | 18 | 57 | Экран выбора уровня (`M8.4`/`M9.1`, срез 2) |
| `docs/LEVEL_SELECT_RENDER.md` | 27 | 75 | Рендер экрана выбора уровня (`engine/src/ui/level_select_render.h`) |
| `docs/LEXE.md` | 15 | 105 | LE/LX-формат: DOS-сборка Gunmetal (`gun.exe`) |
| `docs/LIFT_COLLISION.md` | 16 | 12 | Gunmetal — кабина лифта как движущийся коллайдер |
| `docs/LIGHTMAP_AUDIT.md` | 11 | 8 | Аудит лайтмапов `.sbc`: покрытие и яркость (M4) |
| `docs/LIGHTMAP_EDIT.md` | 22 | 7 | Редактор лайтмапов `.sbc` (`M11.2`, срез 2) |
| `docs/LINE_OF_SIGHT.md` | 17 | 5 | Gunmetal — линия видимости до цели (`M6.2.5`, срез 2) |
| `docs/LOCAL_PLAYERS_RENDER.md` | 15 | 86 | Рендер экрана 21 `NUMBER OF LOCAL PLAYERS` (`engine/src/ui/local_players_render.h`) |
| `docs/LOOK16.md` | 14 | 12 | `LOOK16.TAB`: что делает куб и как свет `0..127` превращается в цвет |
| `docs/LUT_BUILDER.md` | 16 | 67 | Строитель LUT `0x682f90`: 16×256 таблиц воспроизведены из кода (`M8.5`, срез 6) |
| `docs/MAINLOOP.md` | 7 | 209 | Главный цикл и машина состояний (M2.2.2) |
| `docs/MATCH_SETUP_RENDER.md` | 17 | 100 | Рендер экрана 27 `MULTIPLAY GAME OPTIONS` (`engine/src/ui/match_setup_render.h`) |
| `docs/MATERIAL_BLEND.md` | 29 | 159 | Математика материалов полигонов: `none/reflective/translucent/varadd/add/shade` (M4.2.4) |
| `docs/MENU_RENDER.md` | 42 | 57 | Рендер экрана меню (`engine/src/ui/menu_render.h`) |
| `docs/MIPMAPS.md` | 15 | 33 | Mip-уровни, фильтрация и прозрачный индекс |
| `docs/MODDING.md` | 23 | 17 | MODDING — практическое руководство моддера |
| `docs/MOUSE_SETUP_RENDER.md` | 22 | 152 | Рендер экрана 22 `MOUSE SETUP` (`engine/src/ui/mouse_setup_render.h`) |
| `docs/MOVING_COLLIDERS.md` | 29 | 30 | Gunmetal — кинематические коллайдеры дверей и лифтов |
| `docs/MUSIC.md` | 15 | 8 | `audio/music`: громкость и переходы музыкального трека |
| `docs/NESTED_THINGS.md` | 16 | 7 | Вложенные things: статический рекурсивный обход и исполнение |
| `docs/NEW_KEYSET_RENDER.md` | 21 | 133 | Рендер экрана 31 `NAME OF NEW KEY SET` (`engine/src/ui/new_keyset_render.h`) |
| `docs/OBJECTIVES.md` | 24 | 265 | Цели, секреты и триггеры конца уровня (`M6.4.5`) |
| `docs/OBJECTIVES_STATE.md` | 12 | 73 | Маска целей и секреты в движке (`M6.4.5`, срез 2) |
| `docs/ORACLE.md` | 35 | 12 | Эталонный оракул (M4.4.2) |
| `docs/ORACLE_RENDER.md` | 12 | 1 | Воспроизводимый рендер эталонного кадра (`oracle_render.py`) |
| `docs/PAGER_RENDER.md` | 43 | 33 | Рендер страницы SML-пагера (`engine/src/ui/pager_render.h`) |
| `docs/PALETTE.md` | 5 | 15 | Палитра 8-битной графики и индексы текстур `.sbw` |
| `docs/PERFORMANCE.md` | 3 | 0 | PERFORMANCE — измеренные времена (`M12.3`) |
| `docs/PICKUP.md` | 29 | 305 | Предметы: подбор, маркеры регенерации, вирус и щиты (`M6.2.6`) |
| `docs/PICKUP_STATE.md` | 16 | 47 | PICKUP_STATE — правила подбора предметов в движке (`M6.2.6`, срез 3) |
| `docs/PLATFORM_CARRY.md` | 17 | 0 | Gunmetal — движущаяся панель везёт игрока |
| `docs/PLAYER.md` | 28 | 127 | Gunmetal — спавн игрока и первое лицо |
| `docs/PLAYER_CONTROLLER.md` | 28 | 46 | Gunmetal — контроллер игрока (капсула, 91 Гц) |
| `docs/PLAYER_FIELDS.md` | 54 | 1527 | PLAYER_FIELDS — поля рантайм-структуры игрока (`M2.3.1`) |
| `docs/PLAYER_GHIDRA.md` | 30 | 235 | Player speed and gravity: Ghidra notes |
| `docs/PLAYER_GUIDE.md` | 12 | 0 | PLAYER_GUIDE — как играть в переиздание |
| `docs/PLAYER_NAME_RENDER.md` | 21 | 186 | Рендер экранов 24/25, имена локальных игроков (`engine/src/ui/player_name_render.h`) |
| `docs/PLAYER_SETUP_RENDER.md` | 28 | 205 | Рендер экрана 36 `PLAYER 1/2 JOYSTICK SETUP` (`engine/src/ui/player_setup_render.h`) |
| `docs/PLAYLIST.md` | 13 | 3 | `audio/playlist`: списки песен `MUSIC/*.sog` |
| `docs/PLAY_OPTIONS_RENDER.md` | 19 | 138 | Рендер экрана 32 `PLAY OPTIONS` (`engine/src/ui/play_options_render.h`) |
| `docs/PROJECTILE_CPP.md` | 50 | 77 | Снаряды: `movement_type == 6` и RGM (`8`) |
| `docs/QUIT_RENDER.md` | 17 | 105 | Рендер экрана 33 `QUIT GAME?` / `QUIT OPTIONS` (`engine/src/ui/quit_render.h`) |
| `docs/RECORDS.md` | 35 | 260 | Записи состояния `.sbt`: `SECTOR_STUFF` (8), `ITEM` (19), `LIFT` (18) |
| `docs/REGEN.md` | 12 | 12 | Регенерация щитов/брони в тике (`game/regen.*`, `M6.1.3`, срез 4) |
| `docs/REGRESSION.md` | 11 | 1 | Регрессия: ворота, бюджет времени и практики |
| `docs/RELEASE.md` | 4 | 0 | RELEASE — чек-лист релиза (`M12.8`) |
| `docs/RENDER.md` | 5 | 0 | Рендерер C++: конвейер и паритет с `gm/render.py` |
| `docs/RESTART_RENDER.md` | 21 | 149 | Рендер экрана 6 `RESTART GAME` (`engine/src/ui/restart_render.h`) |
| `docs/RE_AUDIT.md` | 23 | 10 | Аудит процесса реверса (2026-10-22) |
| `docs/RE_METHOD.md` | 15 | 8 | Методика реверса `gunmetal-re` |
| `docs/RNG.md` | 7 | 41 | RNG: `rand()` оригинала, сид и потребители |
| `docs/RUNTIME_CPP.md` | 42 | 8 | Gunmetal — C++-каркас фиксированного тика (`GameRuntime`) |
| `docs/SAMPLE_CACHE.md` | 16 | 34 | Рантайм-кэш сэмплов (M7.4): модель, API, числа |
| `docs/SANITIZERS.md` | 4 | 0 | Sanitizers (ASan/UBSan) |
| `docs/SAVE.md` | 19 | 99 | Сохранения Gunmetal: контейнер, блоки, rebuild |
| `docs/SAVE_ACTION.md` | 15 | 0 | Действие «сохранить/загрузить»: `save_action` (`M8.7`/`M9.3`, срез 2) |
| `docs/SAVE_BLOCKS.md` | 15 | 9 | C++: типизированные блоки сейва 1, 2, 3, 6, 7, 8 |
| `docs/SAVE_CPP.md` | 16 | 45 | C++-читатель сейвов Gunmetal |
| `docs/SAVE_FIELDS.md` | 35 | 324 | Поля сейва Gunmetal: карта «поле → смещение → смысл → доказательство» |
| `docs/SAVE_LOAD.md` | 16 | 25 | Сейв → живой рантайм (`save_load`, M8.7/M9, срез 2) |
| `docs/SAVE_MENU.md` | 16 | 19 | Экран сохранений: модель `save_menu` (`M8.4`, срез 2) |
| `docs/SAVE_MENU_RENDER.md` | 34 | 82 | Рендер экрана SAVE GAME (`engine/src/ui/save_menu_render.h`) |
| `docs/SAVE_NAME_RENDER.md` | 16 | 118 | Рендер экрана 15, ввод имени сейва (`engine/src/ui/save_name_render.h`) |
| `docs/SAVE_SLOTS.md` | 11 | 27 | Безголовый список слотов сохранения (`M8.7`, срез 1) |
| `docs/SAVE_WRITER.md` | 12 | 22 | C++-писатель сейвов Gunmetal |
| `docs/SAVE_WRITE_CPP.md` | 19 | 29 | C++-писатель сейва: `write_save()` |
| `docs/SBM_WRITE.md` | 22 | 4 | Запись SBM-архивов: round-trip, аудит и замена записи (`M11.5`, срез 9) |
| `docs/SBT_ASM.md` | 24 | 13 | Ассемблер скриптов `.sbt` (текст → байткод `SCRIPT`) |
| `docs/SBT_RECORDS.md` | 20 | 140 | Все типы записей `.sbt`: раскладка, писатели, побайтовая проверка |
| `docs/SBT_WRITE.md` | 24 | 24 | Писатель контейнера `.sbt` (`M11.4`, часть 1) |
| `docs/SBV.md` | 21 | 17 | `.sbv` — видео/анимации Gunmetal |
| `docs/SBV_PARITY.md` | 9 | 3 | Паритет C++ ↔ Python для `.sbv` на всём корпусе |
| `docs/SBW_EDIT.md` | 23 | 31 | Редактор геометрии `.sbw` (`M11.2`, срез 1) |
| `docs/SBW_MESH.md` | 35 | 13 | Вершины `.sbw`: редактор (`M11.2`, срез 4) |
| `docs/SBW_OBJECTS.md` | 34 | 58 | Объекты `.sbw`: редактор (`M11.2`, срез 3) |
| `docs/SBW_POLY.md` | 41 | 15 | Полигоны `.sbw`: редактор (`M11.2`, срез 5) |
| `docs/SCENE_EDIT.md` | 42 | 131 | Редактор записей сцены `.sbt` (`M11.3`, срез 1) |
| `docs/SCHEMA.md` | 10 | 7 | Схема форматов: единый источник истины (M3.2.1) |
| `docs/SCREEN_CPP.md` | 15 | 5 | SML-экран в C++: один рантайм от `.sme` до RGB |
| `docs/SCREEN_FX.md` | 29 | 350 | Экранные эффекты: фейд-«дымка» (HAZE), вспышка урона, палитра (M4.2.8) |
| `docs/SCRIPT_DEBUG.md` | 32 | 78 | Отладчик и граф скриптов `.sbt` (`M11.4`, срез 1) |
| `docs/SECTOR_LIGHT.md` | 18 | 36 | Скриптовый свет сектора: `sector+0x16` (M4.2.5) |
| `docs/SECTOR_STUFF.md` | 33 | 272 | `SECTOR_STUFF`: какой слот адресует команда (27–31, 40–45) |
| `docs/SECURITY.md` | 12 | 1 | Границы доверия и устойчивость к битым файлам |
| `docs/SHAKE.md` | 30 | 467 | Gunmetal — тряска камеры, view-pitch и отдача |
| `docs/SIM.md` | 8 | 2 | Безголовый прогон уровня (`--sim`, `M6.5.1`) |
| `docs/SIM_DAMAGE.md` | 17 | 16 | Урон игроку, смерть и респавн (`game/sim_player.*`, `M6.5.1`, срез 3) |
| `docs/SIM_PLAYER.md` | 19 | 5 | Игровой тик одного игрока (`game/sim_player.*`, `M6.5.1`, срез 1) |
| `docs/SIM_PROJECTILE.md` | 17 | 6 | Игрок стреляет снарядом: попадание и урон (`game/sim_player.*`, `M6.5.1`, срез 2) |
| `docs/SIM_SCENARIOS.md` | 14 | 0 | Детерминированные сценарии `--sim` (`M9.5`, срез 1) |
| `docs/SKY.md` | 20 | 100 | Фон кадра и «небо»: что рисует движок за геометрией (M4.1.6) |
| `docs/SML_CPP.md` | 12 | 6 | SML/SME в C++: читатель разметки UI |
| `docs/SML_PAGER.md` | 14 | 17 | SML-пагер: runtime-курсор страниц (`engine/src/ui/sml_pager.h`) |
| `docs/SML_WRITE.md` | 22 | 3 | Запись `.sml` / `.sme`: кодек, round-trip и правка текста (`M11.5`, срез 3) |
| `docs/SOA.md` | 28 | 172 | `.soa` — анимации спрайтовых текстур |
| `docs/SOA_WRITE.md` | 27 | 13 | Запись `.soa`: побайтовый round-trip и правка периода (`M11.5`, срез 4) |
| `docs/SOB.md` | 17 | 0 | `.sob` — модели объектов: формат, писатель, round-trip |
| `docs/SOB_WRITE.md` | 21 | 0 | Запись `.sob`: байт-в-байт round-trip и правка токенов (`M11.5`, срез 2) |
| `docs/SOE.md` | 21 | 197 | `.soe` → `THING_TYPES`: семантика полей (M1.6) |
| `docs/SOE_CPP.md` | 13 | 7 | `.soe` → `THING_TYPES`: C++ reader (`engine/src/res/soe.*`) |
| `docs/SOE_WRITE.md` | 27 | 2 | Запись `.soe` и round-trip `.sni` (`M11.5`, срез 8) |
| `docs/SOF_WRITE.md` | 22 | 3 | Запись `.sof`: побайтовый round-trip и правка пикселя (`M11.5`, срез 7) |
| `docs/SOG_WRITE.md` | 18 | 0 | Запись `.sog`: побайтовый round-trip и правка одной строки (`M11.5`, срез 5) |
| `docs/SOO.md` | 29 | 130 | `.soo` — модельные анимации Gunmetal |
| `docs/SOO_PLAYER.md` | 12 | 3 | SOO_PLAYER — runtime-плеер `Sobanim/*.soo` |
| `docs/SOO_WRITE.md` | 31 | 25 | Запись `.soo`: побайтовый round-trip и правка `mode`/длительностей (`M11.5`, срез 6) |
| `docs/SOU1.md` | 12 | 102 | `SOU1`: 4-битный ADPCM Miles — разобран |
| `docs/SOU1_CPP.md` | 10 | 14 | `SOU1` в C++: слой `audio/sou1` |
| `docs/SOU2.md` | 6 | 10 | `SOU2`: музыкальный кодек и правила рендера |
| `docs/SOUND_FIELDS.md` | 24 | 255 | Звук: записи сэмплов и голоса (M2.3.7) |
| `docs/SOUND_NAMES.md` | 27 | 205 | Имена сэмплов: `SOUND_SAMPLE_TABLE` → `SOU.IDX`, поведение при промахе (M7.6) |
| `docs/SOUND_OPTIONS_RENDER.md` | 18 | 111 | Рендер экрана 11 `SOUND OPTIONS` (`engine/src/ui/sound_options_render.h`) |
| `docs/SOUND_POLICY.md` | 14 | 31 | Политика отсутствующих звуков (M7.6, срез 2) |
| `docs/SOU_CONTAINER.md` | 12 | 19 | Контейнер `.sou`: SOU0 / SOU1 / SOU2 |
| `docs/SPRITE.md` | 12 | 0 | Спрайтовый блиттер: индексированные `.sbm` в RGB-кадр |
| `docs/SPRITE_GOLDENS.md` | 6 | 0 | Спрайтовые golden'ы: постоянный Python-эталон |
| `docs/START_GAME_RENDER.md` | 34 | 109 | Рендер экрана START GAME (`engine/src/ui/start_game_render.h`) |
| `docs/SUB.md` | 18 | 102 | `.sub` — таблицы подстановок спрайтов/текстур |
| `docs/SUB_CPP.md` | 13 | 19 | Таблицы подстановок `.sub` (`M2.2`/`M11.5`, срез 2) |
| `docs/TEXTURE_ANIM.md` | 23 | 41 | Скролл и анимация UV полигона (`M4.2.2`, срез 2) |
| `docs/TEXTURE_AUDIT.md` | 12 | 3 | Аудит декодирования текстур `level0` (M4) |
| `docs/TEXT_LAYOUT.md` | 23 | 91 | SML: раскладка текста, шрифтовые атласы и пагинация |
| `docs/TEXUV.md` | 19 | 86 | Текстурные UV кадрового прохода и полигоны без текстуры (M4.1.8) |
| `docs/THINGS_CPP.md` | 32 | 35 | Gunmetal — C++-runtime вещей (`Thing`) |
| `docs/THING_ANIM_BIND.md` | 19 | 21 | `thing_anim` — AI-состояние → клип → кадр (M6.3.1, срез 2) |
| `docs/THING_FIELDS.md` | 54 | 458 | Thing type (`0x776d60`) и thing instance (`0x7ac080`): карта полей (M2.3.3) |
| `docs/TOOLCHAIN.md` | 2 | 1 | Инструменты и версии |
| `docs/TOOLING.md` | 9 | 2 | Инструменты помимо Ghidra: что брать и зачем |
| `docs/UI.md` | 30 | 181 | UI и меню: `.sml` / `.sme`, экраны, ассеты |
| `docs/UI_COVERAGE.md` | 8 | 1 | Покрытие экранов фронтенда (C++) |
| `docs/UI_SCREENS.md` | 30 | 371 | Экраны фронтенда: карта, переходы, load/save, options |
| `docs/UI_SOUNDS.md` | 29 | 313 | Звуки интерфейса и музыка фронтенда (`M8.6`) |
| `docs/UI_SOUNDS_STATE.md` | 10 | 28 | Звуки интерфейса в C++: состояние (`M8.6`, срез 2) |
| `docs/UNDERWATER.md` | 35 | 242 | Подводное состояние, экранный эффект и звук (`M7.7`) |
| `docs/UPSCALE.md` | 23 | 15 | Целочисленный апскейл и палитровый fade (`engine/src/render/upscale.h`) |
| `docs/VIDEO_MODE_LIST_RENDER.md` | 20 | 119 | Рендер экрана 18 `SELECT VIDEO MODE` (`engine/src/ui/video_mode_list_render.h`) |
| `docs/VIDEO_MODE_RENDER.md` | 15 | 106 | Рендер экрана 37 `NEW VIDEO MODE OK?` (`engine/src/ui/video_mode_render.h`) |
| `docs/VM_CPP.md` | 22 | 49 | C++-исполнение SCRIPT-VM (`vm_exec.h` / `vm_exec.cpp`) |
| `docs/VM_EXEC.md` | 55 | 536 | Исполнение SCRIPT-VM (`.sbt`, запись 11) — прототип |
| `docs/VM_SCENE.md` | 25 | 111 | Сцена, созданная ВМ, против сцены из файла (`M5.2`) |
| `docs/VM_TRACE.md` | 5 | 15 | Трасса ВМ: C++ против Python (`M5.3.2`) — закрыто |
| `docs/WEAPONS_CPP.md` | 62 | 320 | `game/weapons.*` — runtime оружия |
| `docs/WEAPON_FIELDS.md` | 26 | 283 | Карта записей «оружие / боеприпасы / оборудование / шасси» (M2.3.5) |
| `docs/WORLD_CENSUS.md` | 20 | 0 | Gunmetal — перепись мира по 36 поставочным уровням |
| `docs/WORLD_FIELDS.md` | 42 | 331 | Мир / сектор / объект / полигон: карта рантайма (M2.3.2) |
| `docs/WORLD_MATERIALS.md` | 34 | 240 | Материалы, оверлеи и флаги мира: потребители полей (M2.3.2) |

## Быстрые команды

```sh
python3 tools/ctx.py ls                      # что вообще есть
python3 tools/ctx.py outline FORMATS         # оглавление и номера строк
python3 tools/ctx.py doc FORMATS sbw         # только раздел про .sbw
python3 tools/ctx.py addr 0x4752d0           # где документирован адрес
python3 tools/ctx.py find apply_weapon_blast --scope code
python3 tools/ctx.py task                    # верх NEXT.md
```
