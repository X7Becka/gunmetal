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
| `game_content.py` | FNV-1a64 канонического содержимого `.sbt`/`.sbw` по манифесту; `--check` ловит переставленные/переименованные файлы, `--write` обновляет `engine/tests/game_content.h` |
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
| `save_rebuild.py` | сейв: `--check`/`--out`/`--diff`, byte-exact rebuild saved0/saved1 |
| `soe_semantics.py` | `.soe`/THING_TYPES: 77 полей, `--fields`/`--json`/`--exe-check` |
| `soo_semantics.py` | `.soo`: mode/loop, ms timeline, контактные листы `--png` |
| `sob_writer.py` | `.sob` writer: `--check` 672 моделей, `--rebuild`, `--edit`, `--self-test` |
| `sou1_model.py` | независимая модель `SOU1` (`0x852b40/0x852c00/0x852cc0`), WAV и сверка: `--verify` (983/983, ~7 мин), `--check docs/manifests/sou1-fnv.json` (быстро), `--list` |
| `campaign.py` | карта переходов кампании/DM из скриптов всех `*.sbt`: `campaign.py [--game DIR] [--json]` (имя, секреты, лучшее время, выходы с условиями) |
| `chassis_models.py` | как уровень выбирает модель игрока: `SOB_TABLE` уровня (восемь моделей шасси по индексам 15–18/21–24), записи `DEFINE_PLAYER_TYPES` (`op5`, звук `op11`) и проверка, что `CHEVY` не встречается нигде |
| `thing_models.py` | модель каждой сущности: `THING_TYPES` (`.sbt`, запись 4) → `+0x1a4` = индекс в `SOB_TABLE`; печать «тип → модель» по уровням и проверка `2896/2896` валидных индексов (`--level`, `--json`) |
| `entities.py` | таблица сущностей `SCRIPT` (колонки «маска / id / смещение подскрипта»): печать строк, проверка стыковки блоков (`15025/15061`), валидности типов `ADD_THING*` (`4005/4005`) и попадания `GENERATE`-id в таблицу; цепочка «id → подскрипт → тип → модель» (`--level`, `--rows`, `--json`) |

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
