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
| `docs/AUDIO_MIXER.md` | 14 | 8 | `audio/mixer`: детерминированный software-микшер |
| `docs/AUDIO_OUTPUT.md` | 16 | 0 | `platform/audio`: вывод звука (SDL3 + null) |
| `docs/BASELINE.md` | 11 | 0 | Baseline: установка, инвентарь и регрессия |
| `docs/BUILD.md` | 4 | 0 | Сборка движка (M3) |
| `docs/CAMERA.md` | 31 | 209 | Gunmetal — камера первого лица: оси, матрица, проекция |
| `docs/CAMERA_CPP.md` | 25 | 74 | `camera/camera.h` — камера первого/третьего лица |
| `docs/COLLIDE.md` | 25 | 10 | Gunmetal — примитивы коллизий `.sbw` |
| `docs/CONFIG.md` | 28 | 241 | `GUNMETAL.CFG` / `CONTROLS.CFG` — карта полей |
| `docs/CONTROLS_CPP.md` | 13 | 19 | `input/controls.h` — `.key` profiles and controller config |
| `docs/DOORS_CPP.md` | 55 | 137 | Gunmetal — C++-модель дверей, переключателей и лифтов |
| `docs/DOOR_FIELDS.md` | 56 | 566 | Поля дверей, переключателей и лифтов (M2.3.4) |
| `docs/ENTITY_FIELDS.md` | 37 | 262 | Скриптовая таблица сущностей (`0x5b0df0`, `0x5e4680`, `0x616658`): карта (M2.3.6) |
| `docs/EVENTS.md` | 24 | 92 | Event queue and `GENERATE` events |
| `docs/FILM.md` | 27 | 107 | HUD-фильмы `.sof` / `.fdx` — формат, привязка, паритет |
| `docs/FORMATS.md` | 202 | 947 | Gunmetal — спецификация форматов данных |
| `docs/FUZZ_CORPUS.md` | 11 | 1 | Фазз-корпус: сохранение и повтор (save / replay) |
| `docs/FUZZ_CPP.md` | 6 | 3 | C++ reader fuzzing (`engine/tests/test_fuzz.cpp`) |
| `docs/GAME_STATE.md` | 15 | 14 | Gunmetal — C++-слой игрового состояния (`GameState`) |
| `docs/GHIDRA.md` | 17 | 30 | Ghidra: выбор, установка и рабочий процесс |
| `docs/HUD_BIND.md` | 17 | 29 | HUD binding: `.sml`/`.sme` экраны → семейства `.sof`/`.fdx` и позиции `<I>` |
| `docs/HUD_COMPOSE.md` | 17 | 9 | HUD composition: `.sml` + `.sof/.fdx` + `blit_indexed` за один вызов |
| `docs/HUD_CPP.md` | 8 | 2 | C++ HUD-слой: блит кадров `.sof` |
| `docs/HUD_PLAYER.md` | 14 | 13 | C++ HUD-фильмы: проигрывание `.sof` + `.fdx` во времени |
| `docs/INIT.md` | 32 | 556 | Инициализация движка: WinMain → видео → звук → ввод → конфиг → палитра (M2.2.1) |
| `docs/INSTALL_AUDIT.md` | 11 | 0 | Аудит установки по манифесту |
| `docs/JOURNAL.md` | 33 | 158 | JOURNAL — журнал ходов |
| `docs/LEVEL_LOAD.md` | 32 | 664 | Загрузка и выгрузка уровня: `.sbw`, `.sbt`, `.sbc`, `SOB_TABLE` (M2.2.3) |
| `docs/LEXE.md` | 15 | 105 | LE/LX-формат: DOS-сборка Gunmetal (`gun.exe`) |
| `docs/MAINLOOP.md` | 7 | 209 | Главный цикл и машина состояний (M2.2.2) |
| `docs/MIPMAPS.md` | 15 | 33 | Mip-уровни, фильтрация и прозрачный индекс |
| `docs/NESTED_THINGS.md` | 16 | 7 | Вложенные things: статический рекурсивный обход и исполнение |
| `docs/ORACLE.md` | 6 | 0 | Эталонный оракул (M4.4.2) |
| `docs/PLAYER.md` | 28 | 127 | Gunmetal — спавн игрока и первое лицо |
| `docs/PLAYER_CONTROLLER.md` | 27 | 46 | Gunmetal — контроллер игрока (капсула, 91 Гц) |
| `docs/PLAYER_FIELDS.md` | 52 | 1483 | PLAYER_FIELDS — поля рантайм-структуры игрока (`M2.3.1`) |
| `docs/PLAYER_GHIDRA.md` | 30 | 235 | Player speed and gravity: Ghidra notes |
| `docs/PROJECTILE_CPP.md` | 50 | 77 | Снаряды: `movement_type == 6` и RGM (`8`) |
| `docs/RECORDS.md` | 33 | 246 | Записи состояния `.sbt`: `SECTOR_STUFF` (8), `ITEM` (19), `LIFT` (18) |
| `docs/REGRESSION.md` | 11 | 1 | Регрессия: ворота, бюджет времени и практики |
| `docs/RENDER.md` | 5 | 0 | Рендерер C++: конвейер и паритет с `gm/render.py` |
| `docs/RE_AUDIT.md` | 13 | 11 | Аудит процесса реверса (2026-10-22) |
| `docs/RE_METHOD.md` | 14 | 7 | Методика реверса `gunmetal-re` |
| `docs/RNG.md` | 7 | 41 | RNG: `rand()` оригинала, сид и потребители |
| `docs/RUNTIME_CPP.md` | 42 | 8 | Gunmetal — C++-каркас фиксированного тика (`GameRuntime`) |
| `docs/SANITIZERS.md` | 1 | 0 | Sanitizers (ASan/UBSan) |
| `docs/SAVE.md` | 19 | 99 | Сохранения Gunmetal: контейнер, блоки, rebuild |
| `docs/SAVE_BLOCKS.md` | 15 | 9 | C++: типизированные блоки сейва 1, 2, 3, 6, 7, 8 |
| `docs/SAVE_CPP.md` | 16 | 45 | C++-читатель сейвов Gunmetal |
| `docs/SAVE_FIELDS.md` | 35 | 321 | Поля сейва Gunmetal: карта «поле → смещение → смысл → доказательство» |
| `docs/SAVE_WRITER.md` | 12 | 22 | C++-писатель сейвов Gunmetal |
| `docs/SBT_RECORDS.md` | 19 | 136 | Все типы записей `.sbt`: раскладка, писатели, побайтовая проверка |
| `docs/SBV.md` | 21 | 17 | `.sbv` — видео/анимации Gunmetal |
| `docs/SBV_PARITY.md` | 9 | 3 | Паритет C++ ↔ Python для `.sbv` на всём корпусе |
| `docs/SCREEN_CPP.md` | 15 | 5 | SML-экран в C++: один рантайм от `.sme` до RGB |
| `docs/SECURITY.md` | 12 | 1 | Границы доверия и устойчивость к битым файлам |
| `docs/SHAKE.md` | 30 | 467 | Gunmetal — тряска камеры, view-pitch и отдача |
| `docs/SML_CPP.md` | 12 | 6 | SML/SME в C++: читатель разметки UI |
| `docs/SOA.md` | 28 | 172 | `.soa` — анимации спрайтовых текстур |
| `docs/SOB.md` | 17 | 0 | `.sob` — модели объектов: формат, писатель, round-trip |
| `docs/SOE.md` | 21 | 197 | `.soe` → `THING_TYPES`: семантика полей (M1.6) |
| `docs/SOE_CPP.md` | 13 | 7 | `.soe` → `THING_TYPES`: C++ reader (`engine/src/res/soe.*`) |
| `docs/SOO.md` | 29 | 130 | `.soo` — модельные анимации Gunmetal |
| `docs/SOO_PLAYER.md` | 12 | 3 | SOO_PLAYER — runtime-плеер `Sobanim/*.soo` |
| `docs/SOU1.md` | 12 | 102 | `SOU1`: 4-битный ADPCM Miles — разобран |
| `docs/SOU1_CPP.md` | 10 | 14 | `SOU1` в C++: слой `audio/sou1` |
| `docs/SOU2.md` | 6 | 10 | `SOU2`: музыкальный кодек и правила рендера |
| `docs/SOUND_FIELDS.md` | 23 | 249 | Звук: записи сэмплов и голоса (M2.3.7) |
| `docs/SOU_CONTAINER.md` | 12 | 19 | Контейнер `.sou`: SOU0 / SOU1 / SOU2 |
| `docs/SPRITE.md` | 11 | 0 | Спрайтовый блиттер: индексированные `.sbm` в RGB-кадр |
| `docs/SUB.md` | 18 | 102 | `.sub` — таблицы подстановок спрайтов/текстур |
| `docs/TEXT_LAYOUT.md` | 23 | 91 | SML: раскладка текста, шрифтовые атласы и пагинация |
| `docs/THINGS_CPP.md` | 32 | 35 | Gunmetal — C++-runtime вещей (`Thing`) |
| `docs/THING_FIELDS.md` | 54 | 458 | Thing type (`0x776d60`) и thing instance (`0x7ac080`): карта полей (M2.3.3) |
| `docs/TOOLCHAIN.md` | 2 | 1 | Инструменты и версии |
| `docs/TOOLING.md` | 9 | 2 | Инструменты помимо Ghidra: что брать и зачем |
| `docs/UI.md` | 30 | 181 | UI и меню: `.sml` / `.sme`, экраны, ассеты |
| `docs/VM_CPP.md` | 20 | 39 | C++-исполнение SCRIPT-VM (`vm_exec.h` / `vm_exec.cpp`) |
| `docs/VM_EXEC.md` | 54 | 530 | Исполнение SCRIPT-VM (`.sbt`, запись 11) — прототип |
| `docs/WEAPONS_CPP.md` | 62 | 320 | `game/weapons.*` — runtime оружия |
| `docs/WEAPON_FIELDS.md` | 26 | 278 | Карта записей «оружие / боеприпасы / оборудование / шасси» (M2.3.5) |
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
