# Запись `GUNMETAL.HI`: побайтовый round-trip и правка одной записи (`M11.5`, срез 11)

* **Статус:** срез готов. Round-trip **1/1** файла побайтово (**9/9** записей,
  576 Б); оракул `tools/re/campaign_flow.py` — **1/1** по записям и по минимуму
  на имя, тулкит `gm/small.py` — **1/1** по записям и по `to_bytes`. Правка
  времени слота 2 (`18` → `5`) меняет **ровно 1 байт** (смещение 128),
  `--check` ок, Python-оракул, `gm.py misc` и C++ `--best-times` видят `5`.
  Переименование слота 8 (`CADTLELTL` → `OIL TERMINAL`) меняет **11 байт**,
  движок видит `OIL TERMINAL 6`. Self-test **40/40**.
* **Ветка:** `feat/hiwrite`, база `master` `6463ee3`, дата 2026-10-24.
* **Файлы:** `tools/editor/hiwrite.py` (новый), `docs/HI_WRITE.md` (этот
  документ). `gm/*`, `gm.py`, `regress.py`, `engine/*`, `ROADMAP.md`,
  `docs/*.md` (кроме этого файла) **не менялись**; C++ не собирался заново.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`, у него тот же SHA). Результаты — в
  `/tmp/opencode/hiwrite` (в git не попадают).
* **Оракулы:** `tools/re/campaign_flow.py` (`parse_hi`, `encode_hi`,
  `best_by_name`) и `gm/small.py` (`parse_hi`, `HighScores.to_bytes`); оба не
  менялись. Читатели проекта: Python `tools/re/campaign_flow.py --hi` и
  `gm.py misc`, C++ `engine/src/ui/records.*` (CLI `--best-times`) и
  `engine/src/game/campaign_state.*` (`load_hi`/`save_hi`).

---

## 1. Раскладка (проверена по самому файлу)

`docs/CAMPAIGN_FLOW.md` §6: файл `<game>/GUNMETAL.HI`, **576 байт = 9 записей
по `0x40`**, запись = `i32 seconds; char name[60]`; имя — не ствол уровня, а
строка `SET_LEVEL_NAME` (`0x45b73c`). То же в `docs/HI_SCORES.md` §1–§2,
`engine/src/ui/records.h` (`kRecordBytes` 0x40, `kNameBytes` 60,
`kShippedRecords` 9), `game/campaign_state.h` (`kHiRecordSize`, `kHiNameSize`,
`kHiRecords`, `kHiFileSize`) и в оракуле
(`tools/re/campaign_flow.py:HI_RECORD`/`HI_NAME`).

Фактическая раскладка файла поставки (побайтовый разбор, не по документации):

```
offset 0x0000  slot 0   u32 le 146   name[60] "TESTING RANGE\0" + 46 нулей
offset 0x0040  slot 1   u32 le 1570  "KELLER CORE\0"        + 48 нулей
offset 0x0080  slot 2   u32 le 18    "TESTING RANGE\0"      + 46 нулей
offset 0x00c0  slot 3   u32 le 109   "POWER STATION\0"      + 46 нулей
offset 0x0100  slot 4   u32 le 18    "NATAKA TOWER\0"       + 47 нулей
offset 0x0140  slot 5   u32 le 7     "NATAKA TOWER\0"       + 47 нулей
offset 0x0180  slot 6   u32 le 11    "OIL RIG\0"            + 52 нуля
offset 0x01c0  slot 7   u32 le 40    "OIL REFINERY\0"       + 47 нулей
offset 0x0200  slot 8   u32 le 6     "CADTLELTL\0"          + 50 нулей
```

| факт | значение | чем проверено |
|---|---|---|
| размер | **576 Б**, кратен `0x40` | `stat`, `parse` |
| записей | **9** | `--audit`/`--list` |
| время | `146, 1570, 18, 109, 18, 7, 11, 40, 6` | `xxd`, `--audit` |
| поле времени | 4 Б, little-endian (`146` → `92 00 00 00`) | `xxd`, self-test |
| имя | ASCII, NUL-терминировано, хвост нулями — **9/9** | `--check` (`ascii=9/9 terminated=9/9 clean-tail=9/9`) |
| длина имён | 7..13 Б | `--audit` (`name-len=7..13`) |
| нулевой хвост | 438 Б в 9 записях (вместе с 9 терминаторами) | `--audit` (`padding=438`) |
| sha256 | `b6393e8cd64c86087ff93170fcb37059ecfd62da3a909dcf7fbe9e24e6c5c019` | `--audit`/`--check` |
| FNV-1a 64 | `0x80bf2f264bebe680` | `--audit`; совпадает с `docs/CAMPAIGN_STATE.md` §4 |
| дубликаты | `TESTING RANGE` ×2, `NATAKA TOWER` ×2 (2 лишние строки) | `--audit` |

Три уточнения к формулировке «u32 время + имя ASCII с нулями»:

1. **Время на диске — u32** (`struct.pack("<I")`), но оба читателя проекта
   берут поле как **`i32`**: `Records::load` отвергает отрицательное значение,
   `campaign_flow.parse_hi` распаковывает `"<i"`. Поэтому значения
   `0x80000000..0xFFFFFFFF` инструмент записывает (это допустимо по полю), но
   помечает предупреждением: такой файл движок отвергнет (проверено, §6).
   Игра таких времён не производит: её часы — `clock / 91` (§6).
2. **Имя — C-строка**, а не «имя + хвост»: байты после первого NUL для игры
   невидимы (сравнение `0x416850` и печать `0x483f00` идут по C-строке).
   В поставке там везде нули, поэтому round-trip побайтовый; файл с мусором
   после NUL инструмент **показывает** (`clean-tail`, `note`), но при
   пересборке этот мусор исчезнет — молчаливой «нормализации» нет, см. §7.
3. **Слоты нумеруются с 0** (0..8), как строки `gunmetal --best-times` и как
   `record=2` в `docs/CAMPAIGN_STATE.md` §4. `--list` печатает и `slot`, и
   байтовое смещение.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/hiwrite.py --roundtrip
```

```
roundtrip: /mnt/d/games/Gunmetal 3dfx
  1 files, 9 records, 576 bytes, 0.004s
  identical=1/1  differs=0  errors=0
  oracle campaign_flow.py: records match=1/1, minimum per name match=1/1
  toolkit gm/small.py: records match=1/1, to_bytes identical=1/1
  tmp: /tmp/opencode/hiwrite-roundtrip-… (removed)
result: OK
```

`--roundtrip` читает файл, **пишет модель обратно в `/tmp`**, читает запись с
диска и сравнивает с исходником побайтово (это проверяет и сериализацию, и
запись). То же на зеркале (`--game ~/.cache/gunmetal-re/game`) — 1/1, 576 Б.
`--roundtrip FILE` проверяет один файл (в том числе правленую копию):
`identical=1/1`; файл с мусором после NUL даёт `identical=0/1 differs=1`,
`problem` нет и код 1 — расхождение объяснено в строке `differs`.

| метрика | результат |
|---|---|
| найдено `*.hi` (рекурсивно, регистр расширения не важен) | 1 |
| **побайтово идентичны после `parse → to_bytes`** | **1/1** (576/576 Б) |
| `campaign_flow.parse_hi` даёт те же записи | 1/1 |
| `campaign_flow.encode_hi` даёт те же байты | 1/1 |
| минимум на имя совпадает с оракулом | 1/1 |
| `gm/small.py` видит те же 9 записей и те же имена | 1/1 |
| `gm/small.py:to_bytes` даёт тот же файл | 1/1 |
| ошибок разбора (не кратно `0x40`) | 0 |
| время | 0,004 с на `/mnt/d`, 0,000 с на зеркале |

Сверка с оракулами — независимая проверка, а не доверие к ним: наш разбор
обязан дать те же записи и тот же минимум, иначе `--roundtrip` печатает
`problem` и возвращает код 1. Оракул работает в `i32`
(`struct.unpack_from("<i")`), поэтому сравнение идёт в `i32`-представлении
(`to_i32`): `0xFFFFFFFF` для него `-1`, байты при `encode_hi` те же.

`gm/small.py` — второй, «сырой» кодировщик: он хранит 60-байтовое поле
целиком (`raw`) и поэтому пересобирает байт в байт даже файл с мусором после
NUL; именно он стоит за проверкой `regress.py` «misc / 10 small formats
round-trip (…/hi)». Наши имена и времена обязаны совпасть с его
`record_name`/`score`, иначе `--roundtrip` печатает `problem` и даёт код 1.

## 3. Числа по поставке (`--audit`)

```sh
python3 tools/editor/hiwrite.py --audit
```

```
audit: /mnt/d/games/Gunmetal 3dfx/GUNMETAL.HI
  576 bytes, 9 records, sha256=b6393e8cd64c8608
  fnv1a64=0x80bf2f264bebe680
  slot seconds  time        name
     0     146  2 : 26      TESTING RANGE
     1    1570  26 : 10     KELLER CORE
     2      18  0 : 18      TESTING RANGE
     3     109  1 : 49      POWER STATION
     4      18  0 : 18      NATAKA TOWER
     5       7  0 : 07      NATAKA TOWER
     6      11  0 : 11      OIL RIG
     7      40  0 : 40      OIL REFINERY
     8       6  0 : 06      CADTLELTL
  times=146,1570,18,109,18,7,11,40,6
  names=TESTING RANGE | KELLER CORE | TESTING RANGE | POWER STATION | NATAKA TOWER | NATAKA TOWER | OIL RIG | OIL REFINERY | CADTLELTL
  filled=9 empty=0 empty-slots=none
  time-range=6..1570 distinct-times=8 name-len=7..13 padding=438
  distinct-names=7 duplicate-rows=2 NATAKA TOWER=2, TESTING RANGE=2
  ascii=9/9 terminated=9/9 clean-tail=9/9
  best: CADTLELTL=6(1), NATAKA TOWER=7(2), OIL RIG=11(1), TESTING RANGE=18(2), OIL REFINERY=40(1), POWER STATION=109(1), KELLER CORE=1570(1)
  note: 2 duplicate row(s): NATAKA TOWER=2, TESTING RANGE=2 -- legal, the game appends and keeps the minimum per name (0x45c8d6)
result: OK
```

Числа сходятся с документацией, посчитанной независимо: 9 записей, времена
`146,1570,18,109,18,7,11,40,6`, минимум `TESTING RANGE = 18` (не 146),
`KELLER CORE = 1570`, `NATAKA TOWER = 7`, 7 имён — `docs/CAMPAIGN_FLOW.md` §6,
`docs/CAMPAIGN_STATE.md` §4, `docs/HI_SCORES.md` §7. Колонка `time` —
доказанный формат `0x483f00` (`"%d : %d%d"`, `0x4bcbdc`; `0` → `UNKNOWN`,
`0x4bcbd4`), например `1570` → `26 : 10`.

`filled`/`empty` — «непустые/пустые слоты»: непустой = в поле имени есть хоть
один байт до NUL. В поставке **9 непустых, 0 пустых**; пустой слот
(все 60 байт нули) — законные данные, `--check` о нём только предупреждает.

## 4. CLI

```sh
python3 tools/editor/hiwrite.py --roundtrip [PATH] [--json] [--keep]
python3 tools/editor/hiwrite.py --audit [FILE] [--json]
python3 tools/editor/hiwrite.py --list [FILE] [--json]
python3 tools/editor/hiwrite.py --check FILE [--against ORIG] [--json]
python3 tools/editor/hiwrite.py --set-time SLOT --seconds N --out OUT
                                 [--in FILE] [--json]
python3 tools/editor/hiwrite.py --set-name SLOT --name TEXT --out OUT
                                 [--in FILE] [--json]
python3 tools/editor/hiwrite.py --self-test [--json]
```

`FILE` по умолчанию — `<game>/GUNMETAL.HI`; `--game` меняет установку
(по умолчанию `/mnt/d/games/Gunmetal 3dfx`). Имя файла ищется
**регистронезависимо**: в поставке он называется `gunmetal.hi`, а C++-читатель
открывает литерал `GUNMETAL.HI`; на drvfs это одно и то же, на ext4 — нет.

`--list` печатает слот, смещение, время, текст времени и имя (непечатаемые
байты экранируются: `\xNN`, `\t`), дубликаты помечены.

`--check` — структура: ровно 576 Б и 9 записей, у каждого имени есть NUL в
60-байтовом поле, имя — printable ASCII. Отдельно печатает числа и «данные»
(дубликаты, пустые слоты, мусор после NUL, время выше `0x7FFFFFFF`, пробелы
по краям) как `note`. `--against ORIG` добавляет побайтовое сравнение:
`IDENTICAL`/`differs`, число изменённых байт (позиционно), первое расхождение,
`delta`, первую изменённую запись с числами «до → после». **Код возврата
`--check` определяется структурой**, а не равенством с `--against`.

Коды возврата: `0` успех, `1` проверка не прошла (`--roundtrip`/`--check`/
`--self-test`), `2` неверный ввод/отказ (сообщение в stderr), `3` внутренняя
ошибка.

## 5. Правка: ровно ожидаемые байты

```sh
python3 tools/editor/hiwrite.py --set-time 2 --seconds 5 --out /tmp/opencode/hiwrite/t5.hi
# edit: …/GUNMETAL.HI -> /tmp/opencode/hiwrite/t5.hi (slot 2/9)
#   time: 18 -> 5; name: 'TESTING RANGE' -> 'TESTING RANGE'
#   bytes: 576 -> 576 (+0); changed 1 byte(s), first at offset 128 (1 inside the record)
#   sha256: b6393e8cd64c8608 -> e2dbc6f6968d1ddb
#   oracle campaign_flow: agrees; --check: ok

python3 tools/editor/hiwrite.py --check /tmp/opencode/hiwrite/t5.hi \
        --against "/mnt/d/games/Gunmetal 3dfx/gunmetal.hi"
# against … (576 bytes): differs
#   changed 1 byte(s) (positional), first diff at offset 128, delta +0, records 9 -> 9
#   first changed slot 2: 18 TESTING RANGE -> 5 TESTING RANGE (1 byte(s) inside the record)
# result: OK
```

`18` → `5` — это одна пара байт `0x12` → `0x05` по смещению `2*0x40 = 128`;
sha256 правленого файла
`e2dbc6f6968d1ddbe2364c55355a358ef4ba8d0ccaa85fac6b0cf764b8553b8d`.

```sh
python3 tools/editor/hiwrite.py --set-name 8 --name "OIL TERMINAL" --out /tmp/opencode/hiwrite/name.hi
# edit: …/GUNMETAL.HI -> …/name.hi (slot 8/9)
#   time: 6 -> 6; name: 'CADTLELTL' -> 'OIL TERMINAL'
#   bytes: 576 -> 576 (+0); changed 11 byte(s), first at offset 516 (11 inside the record)
#   sha256: b6393e8cd64c8608 -> cc1cd17338f5ed66
#   oracle campaign_flow: agrees; --check: ok
#   warning: this rewrites a level name ('CADTLELTL' -> 'OIL TERMINAL'); the other rows of the old name are left as they are
```

11 байт — ровно разница `CADTLELTL` (9 + NUL) и `OIL TERMINAL` (12): смещения
`516..526`. Поле имени всегда пишется целиком: имя + NUL + нули до 60 Б,
поэтому `0x40`-кратность и размер сохраняются.

Правка проверяет пять вещей (иначе `internal error`, код 3):

1. размер файла и все байты вне записи не изменились (число изменённых байт
   вне записи = 0);
2. повторный разбор даёт ровно ожидаемую таблицу: изменённый слот и все
   соседи;
3. `campaign_flow.parse_hi` и `encode_hi` на выходе дают те же записи/байты, а
   минимум на имя совпадает с оракулом;
4. `--check` на выходе не находит структурных проблем;
5. sha256 файла на диске совпадает с отчётом (перечитывается после записи).

`--in FILE` позволяет править уже правленую копию, не трогая установку
(по умолчанию — установка, только чтение).

## 6. Читаемость для проекта (Python и C++)

**Python-тулкит (`gm/small.py`).** Тот же разбор, что в `regress.py`
(«misc / 10 small formats»), через CLI:

```sh
python3 gm.py misc /tmp/opencode/hiwrite/t5.hi
# t5.hi                    576 B  9 records
# misc: 1 parsed, 0 failed                         (код 0)

python3 gm.py misc --json /tmp/opencode/hiwrite/name.hi
# {"file": "name.hi", "type": ".hi", "bytes": 576,
#  "records": [{"score": 146, "name": "TESTING RANGE"}, …,
#              {"score": 6, "name": "OIL TERMINAL"}]}
```

**Python-оракул кампании.** Тот же файл, что у `--roundtrip`, но через CLI:

```sh
python3 tools/re/campaign_flow.py --hi /tmp/opencode/hiwrite/t5.hi --json
# records 9
# times [146, 1570, 5, 109, 18, 7, 11, 40, 6]
# best {'CADTLELTL': 6, 'KELLER CORE': 1570, 'NATAKA TOWER': 7, 'OIL REFINERY': 40,
#       'OIL RIG': 11, 'POWER STATION': 109, 'TESTING RANGE': 5}
```

**C++ (`ui/records`, `--best-times`).** Движок читает `<game>/GUNMETAL.HI`,
поэтому «каталог-копия» в `/tmp` — это только этот файл с именем в верхнем
регистре:

```sh
mkdir -p /tmp/opencode/hiwrite/game
cp /tmp/opencode/hiwrite/t5.hi /tmp/opencode/hiwrite/game/GUNMETAL.HI
/home/nooverload/gunmetal-re/build/sandbox/engine/gunmetal \
        --best-times --game /tmp/opencode/hiwrite/game
# records: rows=9
#   0     146  TESTING RANGE
#   1    1570  KELLER CORE
#   2       5  TESTING RANGE  (duplicate)
#   …
# best: names=7
#   TESTING RANGE          5  0 : 05  entries=2
#   …
```

`--best-times` **не требует других файлов**: в каталоге лежал только
`GUNMETAL.HI`, и чтение прошло (никакого `GUNMETAL.SBT` не понадобилось).
Копия переименованного файла даёт `OIL TERMINAL 6 0 : 06 entries=1` — правка
имени тоже видна читателю.

**Граница `i32`.** Файл с `0xFFFFFFFF` в поле времени (`--set-time 0 --seconds
0xFFFFFFFF`, предупреждение инструмента) движок отвергает:

```
$ gunmetal --best-times --game /tmp/opencode/hiwrite/game3
[error] records: record 0: GUNMETAL.HI: negative time -1        (код 2)
```

а `--check` об этом честно пишет `note` (структура при этом цела, код 0).
Значит, «читаемость» ограничена `0..0x7FFFFFFF`, хотя поле — u32.

## 7. Отказы (код 2) и предупреждения

```
$ … --set-time 0 --seconds 5 --out "/mnt/d/games/Gunmetal 3dfx/x.hi"
hiwrite: error: refusing to write …: it is inside /mnt/d/games/Gunmetal 3dfx (read-only: the game, its mirror and the repository)
# то же для зеркала, корня worktree, главного корня репозитория и --out == --in
$ … --check short.hi                       # 575 Б
hiwrite: error: short.hi is 575 bytes, expected 576 (the shipped table: 9 records of 0x40); this tool edits the shipped table only, the game appends records (0x45c904)
$ … --set-time 0 --seconds 4294967296 --out /tmp/x.hi
hiwrite: error: --seconds 4294967296 is outside 0..0xFFFFFFFF (the field is a u32; the readers use i32)
$ … --set-name 0 --name "CAFé" --out /tmp/x.hi
hiwrite: error: --name 'CAF\udce9' is not ASCII: …
$ … --set-name 0 --name "Z…Z" --out /tmp/x.hi       # 60 байт
hiwrite: error: --name is 60 bytes; at most 59 fit the 60-byte field with its NUL terminator
$ … --set-time 9 --seconds 1 --out /tmp/x.hi
hiwrite: error: --slot 9 is out of range: … has 9 record(s), slots 0..8
```

| случай | поведение |
|---|---|
| файл не 576 Б (575, 640, 0) | отказ (2): инструмент правит поставочную таблицу, игра дописывает |
| файл не кратен `0x40` (только в `--roundtrip`) | строка `error` в отчёте, код 1 |
| время вне `0..0xFFFFFFFF` или не число | отказ (2); `0x...` принимается |
| время `> 0x7FFFFFFF` | запись + предупреждение (читатели `i32` её отвергнут) |
| имя пустое / не ASCII / управляющий байт / >59 Б / с NUL | отказ (2) |
| имя с пробелами по краям | предупреждение (сравнение байтовое) |
| пустой слот получает имя | разрешено; размер и число записей не меняются |
| мусор после NUL в имени | `--check`: `note`; `--roundtrip`: `differs` (модель пишет нули) |
| дубликаты имён | `note`: это данные, игра их дописывает (`0x45c8d6`) |

## 8. Self-test и регрессия

```sh
python3 tools/editor/hiwrite.py --self-test     # 40/40, без файлов игры
python3 tools/editor/hiwrite.py --self-test --json   # то же, JSON: passed/total/checks
python3 regress.py --mirror --json
```

Self-test 40/40: форма `9 × 0x40`, побайтовый round-trip, little-endian поля
времени, NUL+паддинг имени, слоты/смещения, мусор после NUL, имя без NUL,
отказы `load` (575/640), частичная запись в `parse`, `--check` (поставочная
форма, `u32 > i32`, не-ASCII), `--against` (identical/diff), `--list`,
`--audit` (числа и минимумы, отказ на 575), `--set-time` (1 байт, 4 байта,
0 байт при том же значении, диапазон u32, слот -1/9), `--set-name` (запись
поля, не-ASCII, пустое, управляющий байт, 59/60 Б, заполнение пустого слота),
защита `--out` (игра/зеркало/worktree/главный репозиторий/вход), сверка с
`campaign_flow` и `gm/small.py`, ASCII-fold минимума, синтетический
`--roundtrip` (2 файла, 10 записей, 640 Б и один файл: чистый/грязный),
частичный хвост, `esc`,
`diff`/`sha`/FNV-1a, формат времени. Временный каталог — `/var/tmp` (не tmpfs), после прогона удаляется.

## 9. Ограничения

* **Не создаёт и не удаляет записи.** Нет `--add`/`--delete`; файл — всегда
  9 записей по `0x40`. Игра дописывает запись на улучшение (`0x45c904`,
  `"ab"` `0x4b8690`), но это поведение игры, а не этого инструмента.
* **Не проверяет сортировку/уникальность.** Порядок записей — данные: в
  поставке `146` стоит перед `18`, и это нормально (минимум считается по
  имени, `0x45c8d6`). Дубликаты законны.
* **Только поставочная форма 576 Б.** Файл, выросший в игре до 10+ записей,
  отвергается (код 2); писать «десятый слот» инструмент не умеет.
* **Модель C-строки.** Байты после первого NUL при пересборке становятся
  нулями; файл с мусором в хвосте `--roundtrip` пометит `differs`, а `--check`
  — `note`. Это осознанно: игра эти байты не видит, а второй кодировщик
  `gm/small.py` их сохраняет (он пишет `raw` поля как есть) — на поставке
  расхождения нет, 9/9 хвостов чисто нулевые.
* **`u32` против `i32`.** Поле пишется как u32 (`0..0xFFFFFFFF`), но оба
  читателя проекта берут `i32`; значения выше `0x7FFFFFFF` дают
  предупреждение и нечитаемый движком файл (проверено в §6).
* **Имя не сверяется со списком уровней.** `SET_LEVEL_NAME` — строка из
  скрипта, списка в репозитории нет (имена 24 миссий/5 штабов/4 DM/3 секретных
  есть в `docs/CAMPAIGN_FLOW.md` §8, но это не проверка); инструмент
  гарантирует только ASCII и длину ≤ 59 Б. Переименование слота предупреждает,
  что остальные строки старого имени не тронуты.
* **Сравнение имён.** Минимум на имя считается как в C++ (`0x416850`,
  ASCII-fold); оракул `campaign_flow.best_by_name` группирует по точной
  строке. На поставке оба дают те же 7 имён (регистровых двойников нет);
  расхождение возможно только на файле с `OIL RIG`/`oil rig`.
* **Не подключён к CLI движка и CTest**, не меняет `main.cpp`,
  `engine/src/ui/*`, `engine/src/game/*`, `gm/*`, `regress.py`, `ROADMAP.md`.
* **Игра, зеркало и оба корня репозитория защищены**: `--out` внутрь них —
  отказ (2), `--out` == вход — тоже; запись атомарная (`path.tmp` +
  `os.replace`). `--roundtrip` пишет копии только в `/tmp/opencode`
  (`GM_HIWRITE_TMPDIR` переопределяет) и удаляет их, если не задан `--keep`.

## 10. Ссылки

`docs/CAMPAIGN_FLOW.md` §6 (раскладка, `0x45c880`, `0x40dc70`, `0x416850`,
`0x45c8d6`, `0x45c904`, форматы `rb`/`ab`); `docs/HI_SCORES.md` §1–§2 (§7 —
CLI `--best-times`); `docs/CAMPAIGN_STATE.md` §1/§4 (`GUNMETAL.HI` в модели
кампании, FNV и числа round-trip); `engine/src/ui/records.h`/`.cpp`;
`gm.py misc` (CLI тулкита);
`engine/src/game/campaign_state.h` (`load_hi`/`save_hi`, `kHi*`);
`tools/re/campaign_flow.py` (`parse_hi`/`encode_hi`/`best_by_name`);
`gm/small.py` (`parse_hi`/`HighScores.to_bytes`, regress «misc»);
`tools/editor/sogwrite.py`/`docs/SOG_WRITE.md` (тот же приём для `.sog`);
`ROADMAP.md` `M11.5`.
