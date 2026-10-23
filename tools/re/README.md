# `tools/re/` — инструментарий реверса `gundx.exe`

Это «рабочий стол» исследования exe: то, чем снимались раскладки форматов и
семантика скриптовой ВМ. Скрипты ничего не пишут ни в игру, ни в её каталог —
только читают exe и готовый листинг дизассемблера.

## Подготовка

Путь к exe задаётся переменной `GUNMETAL_EXE` (по умолчанию — игра из
`/mnt/d/games/Gunmetal 3dfx`). Листинг дизассемблера (`disrange.py` читает
именно его, `objdump` каждый раз не зовётся) живёт в **постоянном кэше**, а не
в `/tmp` (там tmpfs: `wsl --shutdown` стирал листинг и копию exe):

```sh
python3 tools/re/gmcache.py build     # листинг + бинарный индекс + копия exe
python3 tools/re/gmcache.py status    # что уже есть, sha256 exe
```

Кэш — `~/.cache/gunmetal-re` (переопределяется `GUNMETAL_CACHE`, листинг —
`GUNDX_ASM`). Если листинга нет, `exelib` соберёт его сам из `GUNMETAL_EXE`
(нужен `objdump`). Рядом лежит `gundx.asm.idx` — отсортированные VMA и
смещения строк; `disasm()` ищет диапазон бинарным поиском и читает только
нужные строки (было ~100 мс на запрос, стало ~0,02 мс). Индекс пересобирается
сам, если листинг изменился по размеру/mtime, поэтому устареть молча не может.

Локальное зеркало установки (drvfs медленный: `gm.py sob --check` 1,5 с → 0,07 с):

```sh
python3 tools/re/gmcache.py mirror --sync     # ~65 с первый раз, ~16 с инкремент
python3 tools/re/gmcache.py mirror --check    # size+mtime, --verify — sha256
python3 regress.py --mirror                   # сам синхронизирует при расхождении
```

Зеркало — кэш: `regress.py --mirror` сверяет его с установкой (быстрая проверка
кэшируется на 15 минут, `GUNMETAL_MIRROR_TTL`), при расхождении копирует
заново. Игра по-прежнему только читается.

## Карта PE (этот билд)

`ImageBase = 0x400000`; адреса в документации — полные VA
(`ImageBase + RVA`).

| секция | VA | файл |
|---|---|---|
| `.text` | `0x401000` | `va - 0x400a00` |
| `.rdata` | `0x4a1000` | `va - 0x400a00` |
| `.data` | `0x4a3000` | `va - 0x401a00` |

Ключевые адреса (подробнее — `docs/FORMATS.md` и `NOTES.md`):

| что | адрес |
|---|---|
| таблица букв сигнатур | `0x457204` |
| обходчик сигнатуры | `0x457170` |
| таблица команд `COMMANDS` (192) | `0x4b53e8` |
| таблица условий `CONDITIONS` (59) | `0x4b62e8` |
| диспетчер ВМ (счётчик-курсор `[0x4b6784]`) | `0x457390`, таблица `0x45c500` |
| текстовый загрузчик `.sbt` (ключевые слова) | `0x460eca` |
| потребители таблиц: ammo/weapons/equipment | `0x450b10` / `0x450be0` / `0x450e60` |
| ammo-массив (56 б), weapons (0x2a8 б), equipment | `0x818960` / `0x80fa00` / `0x814c8c` |
| декодер кадров `.sbv` | `0x49ee8c` |

## Модули и утилиты

| файл | назначение |
|---|---|
| `exelib.py` | общий модуль: разбор PE, `va2off`/`off2va`, `cstr`, `strings_in`, `sig_size`, чтение листинга (кэш + индекс) |
| `gmcache.py` | постоянный кэш реверса: листинг/индекс/копия exe, зеркало установки, `status`/`build`/`mirror`/`selftest` |
| `export_parity.py` | экспорт ожидаемых значений Python-парсеров (`sbw`/`sbt`/`sbc`/`sbm` из `SBM.IDX`/`sob`) в текстовый файл — контракт для C++ `test_parity`; `--out`/`--stats`/`--self-test` (12 проверок) |
| `ui_screens.py` | экранный автомат фронтенда: 33 экрана, 34 перехода (таблица `0x4883d8`, id 5..37), слоты сейвов, KEYCFG, UI-ассеты; `--map`/`--assets`/`--json`/`--self-test` (498 проверок) |
| `gameover.py` | game over/титры/рестарт: смерть `action_state=7` и 136 тиков до респавна, `RESTART GAME` (`0x443af9`), титры `CREDITS.SML` на титуле (`0x487d94`, `pager_state` 1→2), `winner.sbv` + `LEVEL0A.SML`, рестарт из штаба `0x4428f0(1)`, экран конца уровня (опкод 91); `--fields`/`--flows`/`--assets`/`--json`/`--self-test` (120) |
| `objectives.py` | цели/секреты/триггеры: 103 → `0x80d7ec`+`0x80d800`, 104 — единственный писатель маски `0x80f9d0` (`0x45be4e`), условие 7, tally `player+0x33cc` (`0x4536b0`), секторные события/урон; `--fields`/`--summary`/`--anchors`/`--json`/`--self-test` (120, 43 якоря) |
| `pickup.py` | предметы: запись `ITEM` (300×0x60), радиусный триггер `0x469770`, взрыв/цепочка, регенерация `DEFINE_REGENERATION_MARKER`, вирус, щиты; `--summary`/`--fields`/`--items`/`--anchors`/`--json`/`--self-test` (76 проверок, 53 якоря) |
| `sound_names.py` | резолв имён сэмплов таблицы `SOUND_SAMPLE_TABLE` → `sounds/SOU.IDX` (регистр, 8 байт, фолбэк в `music`), статистика по 36 уровням, почему `sounds.sub` не работает; `--report`/`--resolve`/`--aliases`/`--json`/`--self-test` (75) |
| `audio_3d.py` | 3D-позиционный звук: неевклидово расстояние `max+0.25(mid+min)`, гейн `volume−atten·2^n·dist >> 2n`, панорама по базису слушателя, приоритеты голосов; `--fields`/`--check`/`--calc`/`--json`/`--self-test` (61) |
| `ui_sounds.py` | звук фронтенда: `UPDOWN`/`SELECT`/`SELECTM`/`MENUON`/`VLEFT` (idx, rate, FNV), карта «событие/действие → звук» с адресами, музыка UI и 3 сайта `play_music`; `--sounds`/`--map`/`--music`/`--json`/`--self-test` (207) |
| `game_content.py` | FNV-1a64 канонического содержимого `.sbt`/`.sbw` по манифесту; `--check` ловит переставленные/переименованные файлы, `--write` обновляет `engine/tests/game_content.h` |
| `install_audit.py` | аудит установки по `docs/manifests/gunmetal-3dfx-136.sha256`: ok/renamed/missing/modified/volatile/extra, группы swap, `--strict`/`--json`/`--jobs`; 2161 файл за 4,3 с; установку только читает |
| `disrange.py` | печать диапазона VA из листинга: `disrange.py 450be0 450d60` |
| `strdump.py` | строки по VA или все строки в диапазоне (`--range`) |
| `xref.py` | поиск dword-ссылок на значение (обычно адрес строки) |
| `cmdtable.py` | дамп таблиц `COMMANDS` / `CONDITIONS` (имя, сигнатура, id, флаг) |
| `sig.py` | размер записи по сигнатуре + сырая таблица классов букв (`--table`) |
| `hexdump.py` | hex+ASCII по VA (`--off` — по файловому смещению) |
| `sbt_fields.py` | разбор `DEFINE_*` таблиц `.sbt` с картой «dword → смещение в памяти» и именами доказанных полей (оружие, боеприпасы, оборудование) |
| `table_layout.py` | восстановление раскладки памяти записи таблицы по её store-функции: `table_layout.py 0x47a170 0x7995e0 --end 0x47a730 --stride 0x1cc` |
| `copy_trace.py` | трассировка блока «копирование записи A в запись B» (напр. шасси → игрок): `copy_trace.py 0x47ac1a 0x47af3e --src 0x7995e0 --stride 0x1cc --dst 0x81bd00 --map "0x47a170 0x7995e0 0x47a730 0x1cc"` |
| `emu.py` | минимальный интерпретатор x86 (по тексту objdump) — оракул для кодека `.sbv`; требует `objdump` |
| `x86mini.py` | мини-интерпретатор x86 по **реальным байтам** exe — исполняет самомодифицирующиеся декодеры `SOU1`/`SOU2` |
| `render_sou2.py` | `SOU2` → слышимый PCM: `0x852d80` → история 413 / lookahead 388 → настоящий `0x8534c0`; `--out-rate 32000`, `--loop` (шов на 2.072 с). Грабли — `docs/SOU2.md` |
| `sou_decode.py` | декод `SOU1` через `x86mini` (эталон): `sou_decode.py INTRO out.wav`, `--list` |
| `sou1_model.py` | независимая модель SOU1 (4-битный ADPCM), `--verify`/`--check` на 983 записях |
| `spawn.py` | `SET_PLAYER_START`/`SET_DEATHMATCH_START` по 36 уровням, высота глаза шасси |
| `camera.py` | базис первого лица `0x41e025`, проекция, `--selftest`, `--json` |
| `records.py` | `SECTOR_STUFF`/`ITEM`/`LIFT`: поля, writer, `--self-test` |
| `sector_stuff.py` | какой слот `SECTOR_STUFF` адресуют 27–31/40–45: формула `index*0x58`, разрешение `SECTOR_HANDLE` из `.sbt`, `--self-test`/`--summary`/`--level`/`--map`/`--json` |
| `save_rebuild.py` | сейв: `--check`/`--out`/`--diff`, byte-exact rebuild saved0/saved1 |
| `soe_semantics.py` | `.soe`/THING_TYPES: 77 полей, `--fields`/`--json`/`--exe-check` |
| `soo_semantics.py` | `.soo`: mode/loop, ms timeline, контактные листы `--png` |
| `sob_writer.py` | `.sob` writer: `--check` 672 моделей, `--rebuild`, `--edit`, `--self-test` |
| `sou1_model.py` | независимая модель `SOU1` (`0x852b40/0x852c00/0x852cc0`), WAV и сверка: `--verify` (983/983, ~2 мин на 16 потоках, `--jobs`), `--check docs/manifests/sou1-fnv.json` (быстро), `--list` |
| `campaign.py` | карта переходов кампании/DM из скриптов всех `*.sbt`: `campaign.py [--game DIR] [--json]` (имя, секреты, лучшее время, выходы с условиями) |
| `chassis_models.py` | как уровень выбирает модель игрока: `SOB_TABLE` уровня (восемь моделей шасси по индексам 15–18/21–24), записи `DEFINE_PLAYER_TYPES` (`op5`, звук `op11`) и проверка, что `CHEVY` не встречается нигде |
| `thing_models.py` | модель каждой сущности: `THING_TYPES` (`.sbt`, запись 4) → `+0x1a4` = индекс в `SOB_TABLE`; печать «тип → модель» по уровням и проверка `2896/2896` валидных индексов (`--level`, `--json`) |
| `entities.py` | таблица сущностей `SCRIPT` (колонки «маска / id / смещение подскрипта»): печать строк, проверка стыковки блоков (`15025/15061`), валидности типов `ADD_THING*` (`4005/4005`) и попадания `GENERATE`-id в таблицу; цепочка «id → подскрипт → тип → модель» (`--level`, `--rows`, `--json`) |

| `equipment.py` | модель экипировки: `DEFINE_EQUIPMENT` (op 175), состояние `+0x3320/+0x3324/+0x3328`, тик `0x4463a9`, эффекты on/off; `--table`/`--stats`/`--effects`/`--json`/`--check`/`--self-test` |
| `effects.py` | отрицательный индекс текстуры = 1-based id таблицы эффектов `0x5642c8` (шаг `0x5c`): 6 117 полигонов, 48 эффектов, 262 пары «уровень×эффект»; `--summary`/`--hist`/`--level`/`--json`/`--self-test` |
| `hud_icons.py` | HUD: иконки 24×24 (`WICON*` оружие, `WCON*` экипировка), геометрия компаса, источники чисел; `--icons`/`--compass`/`--compare`/`--self-test` |
| `hud_compose_ref.py` | эталонная композиция панели кокпита 320×78 из `PANEL`/`FONTS3`/`NUMFONTS`/`WICON*`: `compose(state)`, `--diff`/`--png`/`--json`/`--self-test` |
| `frame_metric.py` | метрика кадра, устойчивая к шуму и сдвигу: Sobel-F1, χ²-колормапа, локальная NCC, композит; `--oracle/--ours`, `--bands`, `--calibrate`, `--old`, `--self-test` |
| `vm_trace_check.py` | трасса ВМ C++ против Python: языконезависимый дамп, сравнение путей как мультимножеств; 36/36 уровней, 328 524 события; `--level`/`--blocks`/`--dump-py`/`--self-test` |
| `oracle_render.py` | кадр из снятого состояния оригинала + сверка с PNG: `--sidecar`, `--uv texel/fit`, `--pitch-offset`, `--self-test` |
| `oracle_fit.py` | подбор камеры кадра (pitch/yaw/focal) по эталону, `--json` |
| `oracle_compare.py` | старая сверка кадра с оригиналом (corr/mad/edges/hist); для вердикта использовать `frame_metric.py` |
| `vm_scene_check.py` | сцена, созданная ВМ, против статической: 36/36, `--summary`/`--json` |
| `screen_fx.py` | экранные эффекты: фейд-заливка (`SET_CAMERA_HAZE` op 80 → `0x80f9a8` → `0x83d288`, alpha 255·haze), вспышка урона как свет модели (`+0x3224`, флаг `0x20`), 6-битная палитра и её модуляция; `--map`/`--scripts`/`--frames`/`--trace`/`--self-test` (85 проверок, 47 якорей) |
| `economy.py` | экономика: `cash` = `+0x332c`, магазины (опкоды 189/190/191/193 → `action_state +0x3210`), правило покупки (`flags&1`, `cash >= E9`), `SELL_ALL`, масса и штраф скорости, респавн (`salvage`); `--fields`/`--summary`/`--saved`/`--anchors`/`--json`/`--self-test` (579 проверок) |
| `campaign_flow.py` | поток кампании: счётчик миссий `0x80f9cc`, 66 сайтов переходов, цели/секреты/лучшее время, финал; `--fields`/`--summary`/`--hi`/`--json`/`--self-test` (35 проверок) |
| `damage.py` | урон/броня/щиты: `0x453b50` (плита → броня, перелив, `flags`), смерть `0x4536e0`, взрыв `0x477280`, цепной `0x4698b0`, вирус, вещи `0x470500`; `--fields`/`--calc`/`--corpus`/`--anchors`/`--json`/`--self-test` (63) |
| `dm_rules.py` | deathmatch: `game_mode` 0/1/2/3, `mp_flags`, счёт `+0x3270` (HUD `0x492fdd`), respawn, гандикап, 248 DM-стартов; `--fields`/`--summary`/`--anchors`/`--json`/`--self-test` (30 проверок, 17 якорей) |
| `doors.py` | двери/переключатели/лифты: раскладка записи двери (флаги, пять id, слова состояния; массив `0x8381a0`), проверка id переключателей по таблице сущностей (`3281/3964`) и позиций по боксу уровня (`--frame`, `3755/3964`); `--level`, `--json` |
Примеры:

```sh
python3 tools/re/cmdtable.py CONDITIONS | head
python3 tools/re/xref.py 0x4b8fc0 0x4b9018          # кто ссылается на ключевые слова
python3 tools/re/sig.py SSSLLLLEEEELLLLDBBDL        # 84 = 0x54
python3 tools/re/sbt_fields.py "/mnt/d/games/Gunmetal 3dfx/dm1.sbt" --table DEFINE_WEAPONS
```

## Про эмулятор

`emu.py` исполняет код игры по дизассемблированному тексту (`objdump -D` на
лету) и использовался как оракул для `Vid/*.sbv` (функции `0x49ee8c`,
`0xa1d000`, `0xa1d940`). Пример запуска — в докстринге модуля; `emu.load_image()`
читает exe, `emu.run(start)` крутит до `emu.SENTINEL`.

## Методика и бюджет контекста

Как добывать и фиксировать факты (лестница доказательств, провенанс,
чеклисты, антипаттерны) — `docs/RE_METHOD.md`. Кратко:

* сначала замер, потом оптимизация; тяжёлое — в `~/.cache/gunmetal-re`,
  `/var/tmp`, `~/build`, не в `/tmp`;
* разбор кэшируется (листинг + индекс), игра читается через зеркало;
* большие доки целиком не читаются — только точечные срезы:

```sh
python3 tools/ctx.py addr 0x4752d0            # где адрес документирован
python3 tools/ctx.py doc FORMATS sbw          # один раздел вместо 207 КБ
python3 tools/ctx.py outline ROADMAP          # оглавление с номерами строк
python3 tools/ctx.py find target_history --scope code --max 10
python3 tools/ctx.py task                     # верх NEXT.md
```

* факт без адреса инструкции и без проверки на данных — не факт;
* `ROADMAP.md` обновляется в том же коммите, `NEXT.md` — в конце хода.
