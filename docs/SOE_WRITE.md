# Запись `.soe` и round-trip `.sni` (`M11.5`, срез 8)

* **Статус:** срез готов. Round-trip **2/2** файлов поставки побайтово
  (`.soe` 1/1 — 340 Б / 67 токенов; `.sni` 1/1 — 3 Б / 1 токен), свой читатель
  + сборка из строковых спанов **2/2**, семантический re-parse **2/2**,
  канонический re-render **2/2**. Self-test **51/51** (без игры). Правка
  `SOBS/railrock.soe`, токен 47 (`f1cc`, proven), `200` → `250` меняет
  **1 байт** (340 → 340 Б), `--check` ок; правка с изменением ширины
  (`200` → `2000`) даёт 341 Б (+1), `--check` ок. Правка `path.sni`
  (`.` → `saves`) даёт 7 Б (+4). `python3 regress.py --mirror --json` —
  **115/115**, `failed: []` (62.2 с).
* **Ветка:** `feat/soewrite`, база `master` `ff87939`, дата 2026-10-24.
* **Файлы:** `tools/editor/soewrite.py` (новый), `docs/SOE_WRITE.md` (этот
  документ). Существующие файлы (`gm/soe.py`, `gm/small.py`, `gm/*.py`,
  `gm.py`, `tools/editor/*`, `tools/re/*`, `regress.py`, `ROADMAP.md`,
  `docs/*.md`, `engine/*`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/soewrite`
  (в git не попадают). C++-движок собран из этого worktree в
  `/var/tmp/gm-soewrite-build` (headless, `-DGM_WITH_SDL3=OFF`), в репозиторий
  ничего не писалось.
* **Оракул:** `gm/soe.py` (`parse_text`, `ThingType.to_bytes`,
  `render_lines`, `FIELD_MAP`), раскладка — `docs/FORMATS.md` §13,
  семантика полей — `docs/SOE.md`; для `.sni` — `gm/small.py`
  (`parse_sni`/`sni_to_bytes`), раскладка — `docs/FORMATS.md` §8.2.

> **Число файлов.** В поставке v1.36 ровно **1** `.soe`
> (`SOBS/railrock.soe`, 340 Б, 61 строка, 67 токенов) и **1** `.sni`
> (`path.sni`, 3 Б). Обход корня игры рекурсивный и регистронезависимый по
> расширению; `--roundtrip`/`--audit` считают оба формата вместе.

---

## 1. Раскладка и что здесь можно править

`.soe` — плоский поток токенов через пробелы/переводы строк
(`docs/FORMATS.md` §13). Первый токен — версия 0..13; дальнейшие блоки
включаются по ней, поэтому грамматика — это последовательность `fscanf`
загрузчика `0x47cb6c`:

```
version
%ld %ld %s %ld %ld                     id / имя модели (SOB_TABLE)
[v>=9]  %ld
%s %lf×6 %s                            тип движения, 6 double, форма
[ellipsoid] %lf×3 | [sphere] %lf       радиусы
%lf×4                                  0x50/0x58/0x60/0x68
[v>=7] %s %ld | %s %s %ld              event_a / (+event_b), 0x1d4
[v>=8] %ld×4 | %ld                     щиты 0..3
%s %s                                  tex_a / tex_b
%ld                                    expire (0x1e8)
[v>=3] %s                              expire event
%s %lf %lf %lf %ld %s %s %ld %ld %ld   destroy/death/sound + 0xc8-блок
[v>=4] %lf   [v>=5] %s   [v>=6] %s×3   fear / create / morale,timeout,damaged
[v>=7] %ld×2  [v>=10] %lf×2  [v>=11] %lf×2  [v>=12] %lf×2  [v>=13] %s %lf
[v>=2] %ld, затем count×%s             path points (≤32)
%ld, затем count×(%s %ld×6)            weapons (≤8)
[v>=1] %ld, затем count×(%s %lf×6 %ld) children (≤4)
```

В поставочном файле числа записаны как ` 13 ` — с ведущим и хвостовым
пробелом в строке (42 такие строки), часть токенов — отдельными строками.
Поэтому инструмент не «переформатирует» файл, а **правит байты одного
токена**: разделители, CRLF и хвостовой перевод строки остаются как были.
Длина файла меняется только на разницу ширины значения (`200` → `2000` —
+1 байт).

Что **не** редактируется (это гейты грамматики, а не поля):

| токен | почему |
|---|---|
| `version` | включает/выключает целые блоки (0..13) |
| `collision_shape` | `sphere` читает 1 радиус, `ellipsoid` — 3 |
| `path_point_count` | число токенов после него (загрузчик ≤ 32) |
| `weapon_count` | число записей оружия (≤ 8) |
| `children_count` | число записей детей (≤ 4) |

Изменение любого из них добавляет/убирает токены — это создание или удаление
блока, а не правка поля. Всё остальное — числа (`%ld`/`%lf`) и строки
(`%s`) — правится: грамматика от значения не зависит. Строковые токены —
это ссылки/имена (`class_name`, `collision_type`, события, имена оружия и
детей); их правка валидна грамматически, но меняет то, что движок будет
искать в своих таблицах.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/soewrite.py --roundtrip
```

```
roundtrip: identical=2/2, 343 bytes, 68 tokens, 0.005 s (/mnt/d/games/Gunmetal 3dfx)
  .soe: identical=1/1, 340 bytes, 67 token(s)
  .sni: identical=1/1, 3 bytes, 1 token(s)
  own reader: 2/2 line-span re-join; semantic re-parse 2/2; canonical re-render 1/1 .soe
result: OK
```

На зеркале (`--mirror`) — то же за 0,001 с.

| метрика | результат |
|---|---|
| найдено `.soe` / `.sni` | 1 / 1 |
| **побайтово после read → oracle `to_bytes` → write → read** | **2 / 2** (343 Б) |
| свой разборщик доходит до конца | 2 / 2 |
| сборка файла из строковых спанов (`rejoin`) | 2 / 2 |
| семантический re-parse записанного файла | 2 / 2 |
| канонический re-render (`render_lines`) даёт те же значения | 1 / 1 (только `.soe`; `.sni` — одна строка, n/a) |
| время | 0,004–0,009 с на `/mnt/d`, 0,001 с на зеркале |

Три независимых уровня проверки:

1. **`rejoin`** — файл собирается из `data[..line.start] + line.text +
   line.eol`, то есть строковые спаны полны и не пересекаются. На этом стоит
   байтовая правка.
2. **Оракул** — `gm.soe.ThingType.to_bytes()` (для `.sni` —
   `gm.small.sni_to_bytes`) пересобирает файл в scratch-каталог; он читается
   с диска и сравнивается с оригиналом побайтово. Запись идёт через
   `mkstemp` + `fsync` + `os.replace`.
3. **Семантика** — записанный файл разбирается заново своим читателем и
   оракулом: метки, типы и значения всех 67 токенов совпадают. Для `.soe`
   дополнительно проверяется, что канонический `render_lines()` (по значению
   на строку) переparсится в те же значения — то есть в файле нет данных,
   которые не выражаются токенами.

## 3. Аудит чисел

```sh
python3 tools/editor/soewrite.py --audit
```

```
audit: 2 file(s) (.soe 1, .sni 1), 343 bytes, 68 tokens, 0.007 s (/mnt/d/games/Gunmetal 3dfx)
  path.sni: 3 bytes, 1 line, EOL '\r\n', path '.'
  SOBS/railrock.soe: 340 bytes, 61 lines, 67 tokens, version 13, collision PUSHABLE/ellipsoid
    tokens by block: header 1, id 6, collision 11, events 8, weapons 8, children 1, path 1, fields 31 (weapon entries 1, children 0, path points 0)
    ints: 27 value(s), 0..116982, 9 distinct; floats: 24 value(s), 0.0..115.0, 5 distinct
    strings: 16 value(s), 6 distinct: 0, NONE, PUSHABLE, ellipsoid, push_crate, railrock
result: OK
```

Расшифровка:

* **67 токенов** = 27 `%ld` + 24 `%lf` + 16 `%s`; блоков: заголовок (версия)
  1 токен, id/модель 6 (включая `f1b0` при v≥9), коллизия 11 (тип движения +
  6 double + форма + 3 радиуса), события 8 (`event_a`, `expire_event`,
  `create_event`, `morale_failure_event`, `time_out_event`, `damaged_event`
  и атрибутированные `str_c8`/`str_f0` — destroy/death, статус «гипотеза»),
  оружие 8 (счётчик + 1 запись × 7), дети 1 (счётчик, записей 0), path 1
  (счётчик, точек 0), прочие поля 31.
* **Числа**: целых 27 значений, диапазон `0..116982` (`f1ac`), 9 различных;
  дробных 24 значения, `0.0..115.0` (радиусы `radius_x`/`radius_y`), 5
  различных. Все числа — в 32-битном поле загрузчика, `%lf` — конечные.
* **Строки**: 16 токенов, 6 различных: `0`, `NONE`, `PUSHABLE`,
  `ellipsoid` (загрузчик приводит форму к нижнему регистру), `push_crate`
  (имя оружия), `railrock` (`class_name`).
* **`path.sni`** — одна строка `.` с CRLF, 3 байта; других строк нет.

## 4. Как правится один токен

1. `split_lines` + `flat_tokens` дают **байтовые спаны** каждого токена
   (индекс в потоке, номер строки, `start..end`). Разбиение — по
   `str.isspace()`, как `str.split()` оракула; latin-1 = один байт на символ.
2. `walk_soe` — собственный разборщик грамматики (независимый от
   `gm/soe.py`): присваивает каждому токену `kind` (`int`/`flt`/`str`),
   `label` (то же имя, что у оракула) и `value`; проверяет версию 0..13,
   целостность чисел, лимиты счётчиков (32/8/4) и то, что после грамматики не
   осталось «хвоста».
3. `resolve_soe_token` берёт токен по номеру и отвергает хвостовые токены
   (`extra`), которые загрузчик не читает.
4. `validate_soe_edit` отвергает структурные токены (таблица §1), пустые
   значения, пробелы внутри значения и нечисловой текст в числовом слоте;
   диапазон — 32-битное поле.
5. `splice` собирает файл из исходных байтов, подменяя только span целевого
   токена.
6. `verify_soe_edit` (11 проверок, все обязательны):
   * свой читатель и оракул разбирают новый файл, `extra == 0`;
   * число токенов не изменилось;
   * изменение размера равно разнице ширины токена;
   * **все байты вне целевого токена идентичны** (включая сдвинутый хвост при
     изменении ширины);
   * текст всех остальных токенов идентичен;
   * у токена тот же `label`/`kind` и у своего читателя, и у оракула;
   * свой читатель и оракул видят записанное значение;
   * структурные поля (`version`, форма, три счётчика) не изменились;
   * число изменённых байт посчитано и напечатано.
7. Файл пишется только после успешной проверки (атомарно).
8. Для `.sni` те же шаги: один токен (вся строка), проверяется сохранение
   терминатора (CRLF), одной строки и байтов вне строки (8 проверок).

**`changed_bytes` против `file aligned diff`.** «Изменено N байт» — это
разница **внутри токена**: `200` → `250` = 1 байт, `200` → `2000` = 1 байт
(вставка) при `size_delta +1`. Второе число (`file aligned diff`) считает
побайтовое сравнение файлов от начала: при изменении ширины оно включает
сдвинутый хвост (у `2000` — 86), поэтому печатается отдельно и не выдаётся
за «изменённые байты токена».

## 5. CLI

```sh
python3 tools/editor/soewrite.py --list FILE [--json]
python3 tools/editor/soewrite.py --check FILE [--against ORIG]
                                     [--token N --value V] [--json]
python3 tools/editor/soewrite.py --set FILE --token N --value V --out OUT [--json]
python3 tools/editor/soewrite.py --roundtrip [DIR] [--mirror] [--verbose] [--json]
python3 tools/editor/soewrite.py --audit [DIR] [--mirror] [--json]
python3 tools/editor/soewrite.py --self-test [--json]
```

`FILE` — путь или имя (`SOBS/railrock.soe`, `railrock`, `path.sni` —
ищется в `SOBS/`, `thing/`, корне игры; регистр расширения не важен).
`--game` по умолчанию `/mnt/d/games/Gunmetal 3dfx` (или `$GUNMETAL`).
`--roundtrip`/`--audit` без аргумента обходят корень игры рекурсивно; явный
`DIR` заменяет его, `--mirror` подставляет зеркало.

`--list` печатает все токены построчно с номером, строкой, байтовым спаном,
типом, меткой и значением:

```
file: /mnt/d/games/Gunmetal 3dfx/SOBS/railrock.soe (340 bytes, 61 line(s), 67 token(s), version 13)
   #  line  span          kind  label                value
   0     1  1..3          int   version              '13'
   ...
  47    48  252..255      int   f1cc                 '200'
```

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод/отказ
(сообщение в stderr), `3` внутренняя ошибка. Трассировка наружу не выходит.

Отказы (проверены self-test'ом и вручную):

```
$ … --set SOBS/railrock.soe --token 0 --value 12 --out /tmp/opencode/soewrite/x.soe
soewrite: version is not editable: it gates whole blocks (0..13); changing it
adds or removes tokens instead of editing one
# exit 2

$ … --set SOBS/railrock.soe --token 14 --value sphere --out …
soewrite: collision_shape is not editable: it gates the radii (sphere = 1,
ellipsoid = 3 values); changing it adds or removes tokens instead of editing one
# exit 2

$ … --set SOBS/railrock.soe --token 67 --value 1 --out …
soewrite: token 67: the file has 67 token(s) (0..66)
# exit 2

$ … --set SOBS/railrock.soe --token 47 --value abc --out …
soewrite: token 47 (f1cc) is an integer, got 'abc'
# exit 2

$ … --set SOBS/railrock.soe --token 47 --value 250 \
      --out "/mnt/d/games/Gunmetal 3dfx/SOBS/x.soe"
soewrite: refusing to write inside the game installation: … (write to
/tmp/opencode or /var/tmp)
# exit 2 — то же для зеркала (~/.cache/gunmetal-re/game) и репозитория
# (worktree и ~/gunmetal-re)

$ … --set SOBS/railrock.soe --token 47 --value 250 \
      --out /tmp/opencode/soewrite/x.txt
soewrite: --out must be a .soe path: /tmp/opencode/soewrite/x.txt
# exit 2

$ … --list x.txt                       # существующий файл не того типа
soewrite: not a .soe/.sni file: x.txt
# exit 2
```

Дополнительно отвергаются: пустой файл, битая грамматика (нехватка токенов,
нечисло в числовом слоте, версия вне 0..13, счётчик больше потока),
`.sni` из двух строк, NUL в пути, `--out` равный входу, одиночный
`--token`/`--value` в `--check`.

## 6. Правка на реальных данных

### `SOBS/railrock.soe`, поле `f1cc` (proven, `target_zone_threshold`)

Токен 47 (строка 48, байты `252..255`) — `200`. Меняем на `250` (та же
ширина):

```sh
python3 tools/editor/soewrite.py --set SOBS/railrock.soe --token 47 \
    --value 250 --out /tmp/opencode/soewrite/railrock-f1cc250.soe
```

```
/mnt/d/games/Gunmetal 3dfx/SOBS/railrock.soe -> /tmp/opencode/soewrite/railrock-f1cc250.soe
token 47 (f1cc): '200' -> '250' (340 -> 340 bytes, delta +0; 1 byte(s) in the token, file aligned diff 1)
  [ok  ] own reader parses the edited file (67 token(s), extra 0)
  [ok  ] oracle parses the edited file to EOF (extra 0)
  [ok  ] token count is unchanged (67 -> 67)
  [ok  ] size change equals the token width change (340 -> 340 bytes (+0))
  [ok  ] bytes outside the target token are identical
  [ok  ] every other token text is identical (66 token(s))
  [ok  ] the token keeps its grammar label and kind (#47 f1cc/int)
  [ok  ] own reader sees the new value (250)
  [ok  ] oracle sees the new value (250)
  [ok  ] the structural fields are unchanged (version 13, shape ellipsoid, path 0, weapons 1, children 0)
  [ok  ] changed-byte count is reported (1 byte(s))
wrote /tmp/opencode/soewrite/railrock-f1cc250.soe
result: OK
```

`--check` против оригинала:

```sh
python3 tools/editor/soewrite.py --check /tmp/opencode/soewrite/railrock-f1cc250.soe \
    --against SOBS/railrock.soe --token 47 --value 250
```

```
check: /tmp/opencode/soewrite/railrock-f1cc250.soe (340 bytes, .soe)
  [ok  ] line scanner re-joins the file byte-exactly (61 line(s), final EOL b'\r\n')
  [ok  ] own token count equals the oracle token count (67 vs 67)
  [ok  ] own labels equal the oracle labels (67 label(s))
  [ok  ] own kinds equal the oracle kinds (27 int, 24 flt, 16 str)
  [ok  ] own values equal the oracle values (67 value(s))
  [ok  ] no trailing tokens (the loader consumes the whole stream) (own extra 0, oracle extra 0)
  [ok  ] the version is inside the loader range (13 (0..13))
  [ok  ] the block counts fit the loader clamps (path 0/32, weapons 1/8, children 0/4)
against: /mnt/d/games/Gunmetal 3dfx/SOBS/railrock.soe (340 bytes)
  [ok  ] the file is exactly the source with the token patched (size differs by +0 byte(s), first diff at offset 253; aligned byte diff 1)
  [ok  ] the token carries the expected value (#47 f1cc = '250')
  [ok  ] only the target token differs (#47 only)
changed: 1 byte(s) in the token, file aligned diff 1
result: OK
```

Изменён ровно один байт — `cmp -l` (позиции 1-based):

```
$ cmp -l "…/SOBS/railrock.soe" /tmp/opencode/soewrite/railrock-f1cc250.soe
254  60  65      # '0' -> '5'
```

**Правка с изменением ширины** (`200` → `2000`, +1 байт):

```
token 47 (f1cc): '200' -> '2000' (340 -> 341 bytes, delta +1; 1 byte(s) in the token, file aligned diff 86)
  [ok  ] bytes outside the target token are identical
  [ok  ] only the target token differs (#47 only)
result: OK
```

`cmp -l` показывает сдвиг хвоста, но `--check` доказывает, что хвост
побайтово тот же (сравнение «файл == источник с пропатченным токеном»).

### `path.sni` (`.`, 3 Б)

```sh
python3 tools/editor/soewrite.py --set path.sni --token 0 --value saves \
    --out /tmp/opencode/soewrite/path-saves.sni
```

```
/mnt/d/games/Gunmetal 3dfx/path.sni -> /tmp/opencode/soewrite/path-saves.sni
token 0 (path): '.' -> 'saves' (3 -> 7 bytes, delta +4; 5 byte(s) in the token, file aligned diff 7)
  [ok  ] own reader parses the edited file (5 char(s))
  [ok  ] the file still holds exactly one path line (1 line(s))
  [ok  ] the line terminator is preserved (b'\r\n')
  [ok  ] size change equals the path width change (3 -> 7 bytes (+4))
  [ok  ] bytes outside the path line are identical
  [ok  ] own reader sees the new path ('saves')
  [ok  ] the oracle sees the new path ('saves')
  [ok  ] changed-byte count is reported (7 byte(s))
result: OK
```

Результат: `73 61 76 65 73 0d 0a` — `saves\r\n`, терминатор сохранён.

## 7. Читает ли движок

**`.soe` — да.** В C++ есть `gm::parse_soe` (`engine/src/res/soe.cpp`),
`--dump` (`engine/src/main.cpp:472`) и CTest `soe`. C++-код не менялся;
проверка — бинарь, собранный из этого worktree
(`/var/tmp/gm-soewrite-build/engine/gunmetal`, SDL3 OFF):

```sh
$B --dump "…/SOBS/railrock.soe"
$B --dump /tmp/opencode/soewrite/railrock-f1cc250.soe
```

```
file: /mnt/d/games/Gunmetal 3dfx/SOBS/railrock.soe
size: 340 bytes
kind: SOE thing template
thing: railrock id: railrock version: 13
fields: 67 extra: 0
map: 77 fields

file: /tmp/opencode/soewrite/railrock-f1cc250.soe
size: 340 bytes
kind: SOE thing template
thing: railrock-f1cc250 id: railrock version: 13
fields: 67 extra: 0
map: 77 fields
```

`thing:` у правленого файла — это стем имени файла (загрузчик копирует его в
`name[16]`), а не поле из потока; `id`/`version`/`fields`/`extra` пришли из
файла. `--dump` не печатает отдельные поля, поэтому значение проверено
одноразовой программой в `/var/tmp` (не в репозитории), которая линкует
настоящий `engine/src/res/soe.cpp` и вызывает `ThingType::get_int`:

```sh
zigcxx -std=c++20 -O1 -Iengine/src /var/tmp/soe_probe.cpp \
       engine/src/res/soe.cpp -o /var/tmp/soe_probe
```

```
railrock.soe:            fields=67 extra=0 complete=1 version=13 id=railrock f1cc=200   maxhp=0 class=railrock
railrock-f1cc250.soe:    fields=67 extra=0 complete=1 version=13 id=railrock f1cc=250   maxhp=0 class=railrock
railrock-f1cc2000.soe:   fields=67 extra=0 complete=1 version=13 id=railrock f1cc=2000  maxhp=0 class=railrock
```

То есть движковый парсер читает правленый файл целиком (`extra=0`) и видит
новое значение `f1cc`.

**`.sni` — нет.** В C++-порте читателя `.sni` нет (ни в `engine/src`, ни в
CTest; `grep -rn "sni" engine/` пуст), поэтому для него доказательство
ограничено Python-оракулом `gm/small.py` и round-trip'ом. В игре файл, судя
по имени (`path.sni` = одна строка `.`), задаёт путь к каталогу ресурсов;
этот вывод в срезе не проверялся на оригинале.

## 8. Ограничения

* **Блоки не создаются и не удаляются.** Правка одного токена; `version`,
  форма и три счётчика read-only, поэтому число токенов всегда остаётся
  прежним. Новый `.soe` «с нуля» — вне среза.
* **Смена `version`/`collision_shape` невозможна** даже на «эквивалентное»
  значение: она меняет число читаемых токенов (`sphere`/`ellipsoid`,
  блоки по версии). Такой файл надо собирать целиком, а не патчить.
* **Строковые токены правятся как текст**, без проверки, что имя есть в
  таблицах движка: `class_name` ищется в `SOB_TABLE`, `collision_type` — в
  таблице из 19 имён (`0x46c840`), события/оружие/дети — в своих таблицах.
  Неверное имя — валидный файл с другим смыслом.
* **Round-trip `.soe` доказывается через `ThingType.to_bytes()`** —
  собственного сериализатора у инструмента нет; его вклад — точные спаны
  (`rejoin`), независимый разборщик и байтовый splice. `to_bytes()` обратен
  для поставочного файла, но это line-based writer: он сохраняет строки,
  а не пересобирает их из значений (`render_lines()` — канонический вид,
  только для проверки семантики).
* **`.sni`-оракул нормализует терминатор:** `sni_to_bytes` всегда пишет
  CRLF и отбрасывает хвостовые `\r\n`. Поставочный файл — `.\r\n`, поэтому
  round-trip 1/1; LF-only/без терминатора файл читается, но `structure`
  выдаёт warning и round-trip честно покажет расхождение с причиной.
* **Числа проверяются как 32-битные** (`%ld`) и конечные `double` (`%lf`);
  более узкие поля записи `THING_TYPES` (например `+0x200` — volume) этим
  инструментом не ограничиваются: диапазон задаёт загрузчик, а не срез.
* **`--audit` группирует токены по смыслу** (`docs/FORMATS.md` §13):
  `events` включает `str_c8`/`str_f0` как destroy/death — это гипотеза
  (`docs/SOE.md` §3), а не строка ошибки загрузчика.
* **В игре поведение не проверялось** (игра только на чтение): проверены
  Python-оракул, свой читатель и C++-парсер/`ThingType` на реальном файле.
* **JSON** — один объект на режим (`--json`), без схемы-версии; поля
  перечислены в коде (`roundtrip`, `audit`, `list`, `check`, `set`,
  `self-test`).

## 9. Воспроизведение

```sh
cd /home/nooverload/build/gm-sub-soewrite
python3 tools/editor/soewrite.py --self-test            # 51/51, без игры
python3 tools/editor/soewrite.py --roundtrip            # 2/2, /mnt/d
python3 tools/editor/soewrite.py --roundtrip --mirror   # 2/2, зеркало
python3 tools/editor/soewrite.py --audit                # числа §3
python3 tools/editor/soewrite.py --list SOBS/railrock.soe
python3 tools/editor/soewrite.py --set SOBS/railrock.soe --token 47 \
    --value 250 --out /tmp/opencode/soewrite/railrock-f1cc250.soe
python3 tools/editor/soewrite.py --check /tmp/opencode/soewrite/railrock-f1cc250.soe \
    --against SOBS/railrock.soe --token 47 --value 250
python3 tools/editor/soewrite.py --set path.sni --token 0 --value saves \
    --out /tmp/opencode/soewrite/path-saves.sni
python3 regress.py --mirror --json                      # 115/115, 62.2 с
```

`--self-test` не обращается к игре: все потоки (v13, v0, sphere/ellipsoid,
LF/CRLF/одна строка, битые и обрезанные варианты) строятся из синтетических
токенов, правки и отказы проверяются во временном каталоге.
