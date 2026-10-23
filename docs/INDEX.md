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
| `docs/AUDIO_3D.md` | 36 | 311 | 3D-позиционный звук (M7.3): очередь, слушатель, формулы |
| `docs/AUDIO_MIXER.md` | 14 | 8 | `audio/mixer`: детерминированный software-микшер |
| `docs/AUDIO_OUTPUT.md` | 16 | 0 | `platform/audio`: вывод звука (SDL3 + null) |
| `docs/AUDIT_NUMBERS.md` | 21 | 64 | AUDIT_NUMBERS — независимая сверка чисел в свежих RE-доках |
| `docs/BASELINE.md` | 12 | 0 | Baseline: установка, инвентарь и регрессия |
| `docs/BIGFILE_WRITE.md` | 17 | 46 | BIGFILE_WRITE — запись архивов `.IDX`/`.DAT` (`M11.6`) |
| `docs/BUILD.md` | 4 | 0 | Сборка движка (M3) |
| `docs/CAMERA.md` | 31 | 209 | Gunmetal — камера первого лица: оси, матрица, проекция |
| `docs/CAMERA_CPP.md` | 25 | 74 | `camera/camera.h` — камера первого/третьего лица |
| `docs/CAMPAIGN_FLOW.md` | 35 | 302 | Поток кампании: счётчик миссий, штаб, цели, секреты, время, финал (`M9.1`) |
| `docs/CHASSIS.md` | 22 | 236 | Шасси: выбор, 57 CHEVY, модели/звуки, HUD (`M6.1.4`, `M6.5.3`) |
| `docs/COLLIDE.md` | 25 | 10 | Gunmetal — примитивы коллизий `.sbw` |
| `docs/CONFIG.md` | 28 | 241 | `GUNMETAL.CFG` / `CONTROLS.CFG` — карта полей |
| `docs/CONTROLS_CPP.md` | 13 | 19 | `input/controls.h` — `.key` profiles and controller config |
| `docs/DAMAGE.md` | 31 | 255 | Урон, броня и щиты (`M6.5.2`) |
| `docs/DM_RULES.md` | 31 | 394 | Deathmatch: режимы, счёт, respawn, гандикап, команды (`M9.2`) |
| `docs/DOORS_CPP.md` | 55 | 137 | Gunmetal — C++-модель дверей, переключателей и лифтов |
| `docs/DOOR_FIELDS.md` | 56 | 566 | Поля дверей, переключателей и лифтов (M2.3.4) |
| `docs/ECONOMY.md` | 27 | 386 | ECONOMY — деньги, магазины, масса (`M6.2.6`) |
| `docs/EFFECTS.md` | 37 | 198 | Отрицательные индексы текстур: кадры спрайтов/анимаций в кадровом проходе (M4.2.6) |
| `docs/ENTITY_FIELDS.md` | 37 | 262 | Скриптовая таблица сущностей (`0x5b0df0`, `0x5e4680`, `0x616658`): карта (M2.3.6) |
| `docs/EQUIPMENT.md` | 30 | 318 | EQUIPMENT — модель экипировки (`DEFINE_EQUIPMENT`, состояние, эффекты) |
| `docs/EVENTS.md` | 24 | 92 | Event queue and `GENERATE` events |
| `docs/FAQ.md` | 14 | 0 | FAQ — частые вопросы |
| `docs/FILM.md` | 27 | 107 | HUD-фильмы `.sof` / `.fdx` — формат, привязка, паритет |
| `docs/FOG.md` | 22 | 220 | Туман по глубине: формула, таблица `lighttable`, Glide (M4.1.6) |
| `docs/FORMATS.md` | 203 | 950 | Gunmetal — спецификация форматов данных |
| `docs/FRAME_METRIC.md` | 23 | 0 | Метрика сравнения кадров (M4.4.3) |
| `docs/FUZZ_CORPUS.md` | 11 | 1 | Фазз-корпус: сохранение и повтор (save / replay) |
| `docs/FUZZ_CPP.md` | 6 | 3 | C++ reader fuzzing (`engine/tests/test_fuzz.cpp`) |
| `docs/GAME_OVER.md` | 28 | 318 | Game over, титры, рестарт и экран конца уровня (`M9.5`) |
| `docs/GAME_STATE.md` | 15 | 14 | Gunmetal — C++-слой игрового состояния (`GameState`) |
| `docs/GHIDRA.md` | 17 | 30 | Ghidra: выбор, установка и рабочий процесс |
| `docs/HUD_AUDIT.md` | 49 | 160 | HUD кокпита `level0`: инвентарь элементов (M4.4) |
| `docs/HUD_BIND.md` | 17 | 29 | HUD binding: `.sml`/`.sme` экраны → семейства `.sof`/`.fdx` и позиции `<I>` |
| `docs/HUD_COMPOSE.md` | 34 | 43 | HUD composition: `.sml` + `.sof/.fdx` + `blit_indexed` за один вызов |
| `docs/HUD_CPP.md` | 8 | 2 | C++ HUD-слой: блит кадров `.sof` |
| `docs/HUD_ICONS.md` | 27 | 210 | HUD: иконки 24×24, компас и источники чисел (M4.4.5, продолжение) |
| `docs/HUD_PLAYER.md` | 14 | 13 | C++ HUD-фильмы: проигрывание `.sof` + `.fdx` во времени |
| `docs/INIT.md` | 32 | 556 | Инициализация движка: WinMain → видео → звук → ввод → конфиг → палитра (M2.2.1) |
| `docs/INSTALL_AUDIT.md` | 12 | 0 | Аудит установки по манифесту |
| `docs/JOURNAL.md` | 88 | 272 | JOURNAL — журнал ходов |
| `docs/LAYOUT.md` | 8 | 0 | LAYOUT — каталоги, конфигурация, канон |
| `docs/LEVEL_LOAD.md` | 32 | 672 | Загрузка и выгрузка уровня: `.sbw`, `.sbt`, `.sbc`, `SOB_TABLE` (M2.2.3) |
| `docs/LEXE.md` | 15 | 105 | LE/LX-формат: DOS-сборка Gunmetal (`gun.exe`) |
| `docs/LIGHTMAP_AUDIT.md` | 11 | 8 | Аудит лайтмапов `.sbc`: покрытие и яркость (M4) |
| `docs/LOOK16.md` | 14 | 12 | `LOOK16.TAB`: что делает куб и как свет `0..127` превращается в цвет |
| `docs/MAINLOOP.md` | 7 | 209 | Главный цикл и машина состояний (M2.2.2) |
| `docs/MATERIAL_BLEND.md` | 29 | 159 | Математика материалов полигонов: `none/reflective/translucent/varadd/add/shade` (M4.2.4) |
| `docs/MIPMAPS.md` | 15 | 33 | Mip-уровни, фильтрация и прозрачный индекс |
| `docs/MODDING.md` | 23 | 17 | MODDING — практическое руководство моддера |
| `docs/NESTED_THINGS.md` | 16 | 7 | Вложенные things: статический рекурсивный обход и исполнение |
| `docs/OBJECTIVES.md` | 24 | 265 | Цели, секреты и триггеры конца уровня (`M6.4.5`) |
| `docs/ORACLE.md` | 35 | 12 | Эталонный оракул (M4.4.2) |
| `docs/ORACLE_RENDER.md` | 12 | 1 | Воспроизводимый рендер эталонного кадра (`oracle_render.py`) |
| `docs/PALETTE.md` | 5 | 15 | Палитра 8-битной графики и индексы текстур `.sbw` |
| `docs/PERFORMANCE.md` | 3 | 0 | PERFORMANCE — измеренные времена (`M12.3`) |
| `docs/PICKUP.md` | 29 | 305 | Предметы: подбор, маркеры регенерации, вирус и щиты (`M6.2.6`) |
| `docs/PLAYER.md` | 28 | 127 | Gunmetal — спавн игрока и первое лицо |
| `docs/PLAYER_CONTROLLER.md` | 27 | 46 | Gunmetal — контроллер игрока (капсула, 91 Гц) |
| `docs/PLAYER_FIELDS.md` | 54 | 1527 | PLAYER_FIELDS — поля рантайм-структуры игрока (`M2.3.1`) |
| `docs/PLAYER_GHIDRA.md` | 30 | 235 | Player speed and gravity: Ghidra notes |
| `docs/PLAYER_GUIDE.md` | 12 | 0 | PLAYER_GUIDE — как играть в переиздание |
| `docs/PROJECTILE_CPP.md` | 50 | 77 | Снаряды: `movement_type == 6` и RGM (`8`) |
| `docs/RECORDS.md` | 35 | 260 | Записи состояния `.sbt`: `SECTOR_STUFF` (8), `ITEM` (19), `LIFT` (18) |
| `docs/REGRESSION.md` | 11 | 1 | Регрессия: ворота, бюджет времени и практики |
| `docs/RELEASE.md` | 4 | 0 | RELEASE — чек-лист релиза (`M12.8`) |
| `docs/RENDER.md` | 5 | 0 | Рендерер C++: конвейер и паритет с `gm/render.py` |
| `docs/RE_AUDIT.md` | 22 | 10 | Аудит процесса реверса (2026-10-22) |
| `docs/RE_METHOD.md` | 14 | 7 | Методика реверса `gunmetal-re` |
| `docs/RNG.md` | 7 | 41 | RNG: `rand()` оригинала, сид и потребители |
| `docs/RUNTIME_CPP.md` | 42 | 8 | Gunmetal — C++-каркас фиксированного тика (`GameRuntime`) |
| `docs/SANITIZERS.md` | 4 | 0 | Sanitizers (ASan/UBSan) |
| `docs/SAVE.md` | 19 | 99 | Сохранения Gunmetal: контейнер, блоки, rebuild |
| `docs/SAVE_BLOCKS.md` | 15 | 9 | C++: типизированные блоки сейва 1, 2, 3, 6, 7, 8 |
| `docs/SAVE_CPP.md` | 16 | 45 | C++-читатель сейвов Gunmetal |
| `docs/SAVE_FIELDS.md` | 35 | 324 | Поля сейва Gunmetal: карта «поле → смещение → смысл → доказательство» |
| `docs/SAVE_WRITER.md` | 12 | 22 | C++-писатель сейвов Gunmetal |
| `docs/SBT_ASM.md` | 24 | 13 | Ассемблер скриптов `.sbt` (текст → байткод `SCRIPT`) |
| `docs/SBT_RECORDS.md` | 20 | 140 | Все типы записей `.sbt`: раскладка, писатели, побайтовая проверка |
| `docs/SBT_WRITE.md` | 24 | 24 | Писатель контейнера `.sbt` (`M11.4`, часть 1) |
| `docs/SBV.md` | 21 | 17 | `.sbv` — видео/анимации Gunmetal |
| `docs/SBV_PARITY.md` | 9 | 3 | Паритет C++ ↔ Python для `.sbv` на всём корпусе |
| `docs/SBW_EDIT.md` | 23 | 31 | Редактор геометрии `.sbw` (`M11.2`, срез 1) |
| `docs/SCENE_EDIT.md` | 42 | 131 | Редактор записей сцены `.sbt` (`M11.3`, срез 1) |
| `docs/SCREEN_CPP.md` | 15 | 5 | SML-экран в C++: один рантайм от `.sme` до RGB |
| `docs/SCREEN_FX.md` | 29 | 350 | Экранные эффекты: фейд-«дымка» (HAZE), вспышка урона, палитра (M4.2.8) |
| `docs/SCRIPT_DEBUG.md` | 32 | 78 | Отладчик и граф скриптов `.sbt` (`M11.4`, срез 1) |
| `docs/SECTOR_LIGHT.md` | 18 | 36 | Скриптовый свет сектора: `sector+0x16` (M4.2.5) |
| `docs/SECTOR_STUFF.md` | 33 | 272 | `SECTOR_STUFF`: какой слот адресует команда (27–31, 40–45) |
| `docs/SECURITY.md` | 12 | 1 | Границы доверия и устойчивость к битым файлам |
| `docs/SHAKE.md` | 30 | 467 | Gunmetal — тряска камеры, view-pitch и отдача |
| `docs/SIM.md` | 4 | 0 | Безголовый прогон уровня (`--sim`, `M6.5.1`) |
| `docs/SKY.md` | 20 | 100 | Фон кадра и «небо»: что рисует движок за геометрией (M4.1.6) |
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
| `docs/SOUND_FIELDS.md` | 24 | 255 | Звук: записи сэмплов и голоса (M2.3.7) |
| `docs/SOUND_NAMES.md` | 27 | 205 | Имена сэмплов: `SOUND_SAMPLE_TABLE` → `SOU.IDX`, поведение при промахе (M7.6) |
| `docs/SOU_CONTAINER.md` | 12 | 19 | Контейнер `.sou`: SOU0 / SOU1 / SOU2 |
| `docs/SPRITE.md` | 12 | 0 | Спрайтовый блиттер: индексированные `.sbm` в RGB-кадр |
| `docs/SPRITE_GOLDENS.md` | 6 | 0 | Спрайтовые golden'ы: постоянный Python-эталон |
| `docs/SUB.md` | 18 | 102 | `.sub` — таблицы подстановок спрайтов/текстур |
| `docs/TEXTURE_AUDIT.md` | 12 | 3 | Аудит декодирования текстур `level0` (M4) |
| `docs/TEXT_LAYOUT.md` | 23 | 91 | SML: раскладка текста, шрифтовые атласы и пагинация |
| `docs/TEXUV.md` | 19 | 86 | Текстурные UV кадрового прохода и полигоны без текстуры (M4.1.8) |
| `docs/THINGS_CPP.md` | 32 | 35 | Gunmetal — C++-runtime вещей (`Thing`) |
| `docs/THING_FIELDS.md` | 54 | 458 | Thing type (`0x776d60`) и thing instance (`0x7ac080`): карта полей (M2.3.3) |
| `docs/TOOLCHAIN.md` | 2 | 1 | Инструменты и версии |
| `docs/TOOLING.md` | 9 | 2 | Инструменты помимо Ghidra: что брать и зачем |
| `docs/UI.md` | 30 | 181 | UI и меню: `.sml` / `.sme`, экраны, ассеты |
| `docs/UI_SCREENS.md` | 30 | 368 | Экраны фронтенда: карта, переходы, load/save, options |
| `docs/UI_SOUNDS.md` | 29 | 313 | Звуки интерфейса и музыка фронтенда (`M8.6`) |
| `docs/UNDERWATER.md` | 35 | 242 | Подводное состояние, экранный эффект и звук (`M7.7`) |
| `docs/VM_CPP.md` | 22 | 49 | C++-исполнение SCRIPT-VM (`vm_exec.h` / `vm_exec.cpp`) |
| `docs/VM_EXEC.md` | 55 | 536 | Исполнение SCRIPT-VM (`.sbt`, запись 11) — прототип |
| `docs/VM_SCENE.md` | 25 | 111 | Сцена, созданная ВМ, против сцены из файла (`M5.2`) |
| `docs/VM_TRACE.md` | 5 | 15 | Трасса ВМ: C++ против Python (`M5.3.2`) — закрыто |
| `docs/WEAPONS_CPP.md` | 62 | 320 | `game/weapons.*` — runtime оружия |
| `docs/WEAPON_FIELDS.md` | 26 | 283 | Карта записей «оружие / боеприпасы / оборудование / шасси» (M2.3.5) |
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
