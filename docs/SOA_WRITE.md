# Запись `.soa`: побайтовый round-trip и правка периода (`M11.5`, срез 4)

* **Статус:** срез готов. Round-trip **104/104** файлов поставки побайтово
  (8 573 Б, 839 имён), собственный читатель + сборка из строковых спанов
  **104/104**, семантический re-parse **104/104**, таймлайн движка после
  round-trip **102/102** загружаемых, канонический `render_lines()` совпадает
  с файлом лишь у **1/104** — поэтому правка байтовая. Правка
  `anims/Flash3.soa` (задержка кадра 2 = 98 мс) меняет **2 байта** (период
  `20` → `38`, размер 46 → 46), `--check` ок, C++ `parse_soa` (`--dump`) и
  `gm/soa.py` видят новый период и новые интервалы. Self-test **68/68**.
* **Ветка:** `feat/soawrite`, база `master` `53ed29f`, дата 2026-10-24.
* **Файлы:** `tools/editor/soawrite.py` (новый), `docs/SOA_WRITE.md` (этот
  документ). Существующие файлы (`gm/soa.py`, `gm/*.py`, `gm.py`,
  `tools/re/*`, `tools/editor/*`, `regress.py`, `ROADMAP.md`, `docs/*.md`,
  `engine/*`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/soawrite`
  (в git не попадают).
* **Оракул:** `gm/soa.py` (`parse`, `Soa.to_bytes`, `timeline`, `frame_at`),
  раскладка — `docs/SOA.md` §1, семантика — `docs/SOA.md` §2–§4/§6,
  строка формата — `docs/FORMATS.md` §8.

> **Число файлов.** В поставке v1.36 ровно **104** `.soa`:
> `anims/` 77, `spritani/` 26, `Sobanim/` 1 (рекурсивный обход корня игры,
> других каталогов с `.soa` нет). `Sobanim/pulsnuke.SOA` — это `.soa`, а не
> `.soo`; загрузчик ищет его в `anims\`, поэтому файл мёртв (`docs/SOA.md` §9).

---

## 1. Раскладка и почему правка байтовая

`.soa` — обычный текст DOS (CRLF), первая строка — четыре числа, далее по
одному имени спрайта на строку (`docs/SOA.md` §1, `docs/FORMATS.md` §8):

```
<mode> <period> <declared> <loop>
<sprite name>
...
```

* `mode` — 0 (один раз), 1 (цикл), 100/101/102 (часы/фильм, `0x44dc50`);
* `period` — **длительность всего цикла в сотых секунды**, загрузчик хранит
  `period * 91 / 100` тиков (`0x42bad9`), 91 тик = 1 с;
* `declared` — сколько имён читает `fscanf("%s")` (1..32, `0x42baf3`);
* `loop` — мёртвое поле: читается, но рантайм его не смотрит.

Каждое число заголовка — **один токен**, каждая непустая строка после него —
одно имя. Поэтому правка `period`/`mode`/`loop` — замена байтов одного токена,
а всё остальное (CRLF, пустые строки, хвостовые пробелы, наличие/отсутствие
перевода строки в конце) остаётся как было.

Побайтовый round-trip у `gm/soa.py` держится на сохранённых строках:
`Soa.to_bytes()` собирает `b"\r\n".join(lines)`, а `lines = text.split("\r\n")`,
то есть операция обратна разбору для любого CRLF-текста. Канонический
`render_lines()` (то, чем пишется *новая* анимация) даёт побайтово тот же файл
только у **1/104**: у остальных есть хвостовые пустые строки, у
`anims/Conwheel.SOA` имя дополнено пробелами, у `anims/Brief.SOA` и других
пустые строки стоят между именами. Пересборка текста «канонически» изменила бы
эти файлы, поэтому инструмент правит **только байты целевого токена**.

Что измерено на корпусе (`--audit`, §6):

| признак | значение |
|---|---|
| файлов / байт | 104 / 8 573 |
| header-раскладок (ширины токенов) | 5: `x xx x x` 56, `x xx xx x` 23, `x xxx x x` 11, `x xxx xx x` 10, `xxx x x x` 4 |
| пустых строк | 24 |
| строк с хвостовыми пробелами | 1 (`anims/Conwheel.SOA`) |
| файлов без CRLF в конце | 1 |
| bare LF / bare CR | 0 / 0 (все файлы CRLF-only) |
| имён с пробелами внутри | 0 |

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/soawrite.py --roundtrip
```

```
104/104 identical, 8573 bytes, 839 names, 0.28 s (/mnt/d/games/Gunmetal 3dfx)
  own reader: 104/104 parsed, line-span re-join 104/104
  semantic re-parse (parse -> to_bytes -> parse): 104/104
  engine timeline agrees after the round-trip: 102/102 loadable file(s)
  canonical render_lines() identical to the shipped file: 1/104 (the rest keep their layout because to_bytes() re-joins the stored lines)
result: OK
```

Каждый файл: читается своим разборщиком (строки + байтовые спаны), пишется
оракулом (`Soa.to_bytes()`) в scratch-каталог, читается с диска обратно и
сравнивается с оригиналом побайтово.

| метрика | результат |
|---|---|
| найдено `.soa` (рекурсивно от корня игры) | 104 |
| **побайтово идентичны после read → write → read** | **104 / 104** (8 573 Б) |
| свой разборщик доходит до конца | 104 / 104 |
| сборка файла из строковых спанов (`rejoin`) | 104 / 104 |
| семантический round-trip (`parse → to_bytes → parse`) | 104 / 104 |
| таймлайн: свой расчёт == `Soa.timeline()` | 102 / 102 загружаемых |
| канонический `render_lines()` совпал с файлом | 1 / 104 |
| время | 0,28 с на `/mnt/d`, 0,01 с на зеркале (`--mirror`) |

`rejoin` — отдельное доказательство, что строковые спаны полны и не
пересекаются: файл собирается из `data[..line.start] + line.text + line.eol + …`
и обязан совпасть побайтово. На этом стоит байтовая правка. Строки режутся по
CRLF/LF/CR с сохранением терминатора, поэтому «битая» раскладка (bare LF,
отсутствие перевода строки в конце) не «нормализуется» молча.

## 3. Как инструмент правит файл

1. `split_lines` + `tokenize_line` дают **байтовые спаны**: 4 токена заголовка
   и все имена (разбиение по `str.isspace()`, как `str.split()` оракула;
   latin-1 = один байт на символ).
2. `resolve_field` превращает `mode|period|loop` в конкретный токен;
   `validate` сверяет с диапазоном: режимы 0..102 (в поставке 0/1/100/101/102),
   `period` 1..32767 (в поставке 1..600), `loop` 0..1. Для режимов 0/1 период,
   дающий 0 тиков, отвергается: `0x41de7d` делил бы на ноль.
3. `splice` собирает файл из исходных байтов, подменяя только диапазон
   целевого токена. Длина меняется лишь на разницу ширины числа (`20` → `30` —
   0 байт, `20` → `150` — +1 байт).
4. `verify_edit` (10 проверок, все обязательны):
   * свой разборщик и оракул читают новый файл;
   * изменение размера равно разнице ширины токена;
   * число строк не изменилось;
   * остальные три токена заголовка и все имена идентичны;
   * **все байты вне целевого токена идентичны** (включая разделители);
   * свой разборщик и оракул видят записанное значение;
   * для `--delay` — интервал кадра на 91 Гц равен запрошенному, оракул
     согласен, цикл покрывает все кадры;
   * число изменённых байт посчитано и напечатано.
5. `changed_bytes` заново разбирает результат и считает разницу по тексту
   токена (а не по значению, которое «хотели» записать).

### Задержка кадра: отдельного поля нет

В формате **нет** per-frame delay: `period` — длительность всего цикла, а
селектор `0x41de7d` считает кадр как
`(t % period_ticks) * declared / period_ticks` (`docs/SOA.md` §4). Поэтому
`--frame N --delay MS` — это поиск такого `period`, при котором интервал кадра
`N` равен ровно `MS` миллисекунд:

```
interval(k) = ceil((k+1)*p/n) - ceil(k*p/n),  p = period*91/100,  n = declared
hold_ms(k)  = interval(k) * 1000 / 91          (целочисленно, floor)
```

Перебираются `period = 1..32767` (по возрастанию), берётся **наименьший**
период, дающий точное попадание. Если точного попадания нет, правка
**отвергается** с ближайшими представимыми значениями — на 91 Гц держание
кадра квантуется (`… 43, 54, 65, 76, 87, 98, 109 …` мс), и «округлить в
сторону» значило бы записать не то, что просили. Пример из self-test:
4 кадра, задержка 45 мс → отказ, ближайшие 43 и 54 мс.

## 4. CLI

```sh
python3 tools/editor/soawrite.py --list FILE [--limit N] [--json]
python3 tools/editor/soawrite.py --get FILE --field NAME [--json]
python3 tools/editor/soawrite.py --set FILE --period CS --out OUT
python3 tools/editor/soawrite.py --set FILE --frame N --delay MS --out OUT
python3 tools/editor/soawrite.py --set FILE --field NAME=VALUE --out OUT
python3 tools/editor/soawrite.py --check FILE [--against ORIG] [--period CS]
                                       [--field NAME=VALUE] [--strict] [--json]
python3 tools/editor/soawrite.py --roundtrip [DIR] [--mirror] [--json]
python3 tools/editor/soawrite.py --audit [DIR] [--mirror] [--json]
python3 tools/editor/soawrite.py --self-test [--json]
```

`FILE` — путь или имя (`Flash3`, `Flash3.soa`, `DCLOCK1` — ищется в `anims/`,
`spritani/`, `Sobanim/`; регистр не важен). `--game` по умолчанию
`/mnt/d/games/Gunmetal 3dfx` (или `$GUNMETAL`). `--roundtrip`/`--audit` без
аргумента обходят корень игры рекурсивно; явный `DIR` заменяет его, `--mirror`
подставляет зеркало.

Редактируемые поля:

| поле | токенов | смысл |
|---|---:|---|
| `period` | 1 | длительность всего цикла в сотых секунды (`0x42bad9`) |
| `mode` | 1 | 0/1 — обычные, 100..102 — часы/фильм (`0x44dc50`) |
| `loop` | 1 | 0/1; мёртвое поле, правка безопасна, но на поведение не влияет |

Не редактируются осознанно: `declared` (число читаемых имён — это добавление
или удаление кадров), имена кадров (ссылки на спрайты `PICS`/`sprites`),
имя файла/анимации. Формат не хранит id: `local id` в `--list` — это порядковый
номер первого вхождения имени **в этом файле**; настоящий id выдаёт глобальный
реестр `0x76ed40` в порядке загрузки, поэтому он не выводится как «id движка».

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод/отказ
(сообщение в stderr), `3` внутренняя ошибка. Трассировка наружу не выходит.

Отказы (проверены self-test'ом и вручную):

```
$ python3 tools/editor/soawrite.py --set Flash3 --period 30 \
      --out "/mnt/d/games/Gunmetal 3dfx/anims/X.soa"
soawrite: refusing to write inside the game installation: … (write to /tmp/opencode or /var/tmp)
# exit 2 — то же для зеркала (--out в ~/.cache/gunmetal-re/game) и репозитория

$ python3 tools/editor/soawrite.py --set Flash3 --frame 9 --delay 98 \
      --out /tmp/opencode/soawrite/y.soa
soawrite: --frame 9: the animation declares 4 frame(s) (0..3)
# exit 2

$ … --set DCLOCK1 --frame 0 --delay 98 --out …
soawrite: mode 100 is a special mode (100..102): 0x44dc50 ignores the period and
redraws frame 0 every tick, so --delay is meaningless here …
# exit 2

$ … --set copltBLU --frame 0 --delay 98 --out …
soawrite: the engine would refuse this file (declared 10, 6 name(s)); a delay
edit would not be played -- fix the frame list first
# exit 2

$ … --set Flash3 --field declared=5 --out …
soawrite: declared is not editable: it is the number of names the loader reads
(0x42baf3), so changing it adds or removes frames …
# exit 2

$ … --set Flash3 --period 1 --out …
soawrite: period=1: converts to 0 engine ticks; 0x41de7d would divide by zero
(needs period >= 2 for mode 1)
# exit 2 (у режима 100 то же значение 1 допустимо: деления там нет)
```

Дополнительно отвергаются: не `.soa` на вход/выход, `--out` == вход, пустой
файл, заголовок не из четырёх чисел, `declared` вне 1..32, `mode` вне 0..102,
`period` вне 1..32767, файл без имён. Запись атомарная (`mkstemp` в каталоге
цели + `fsync` + `os.replace`).

## 5. Правка на реальных данных: `anims/Flash3.soa`

Файл: `1 20 4 1`, кадры `flash3a..d`, 4 кадра по 18 тиков (5/4/5/4).
Задача — держать кадр 2 по 98 мс (9 тиков):

```sh
python3 tools/editor/soawrite.py --set Flash3 --frame 2 --delay 98 \
    --out /tmp/opencode/soawrite/Flash3-delay98.soa
```

```
/mnt/d/games/Gunmetal 3dfx/anims/Flash3.soa -> /tmp/opencode/soawrite/Flash3-delay98.soa
period: 20 -> 38 (46 -> 46 bytes, delta +0)
frame 2 of 4: requested 98 ms -> period 38 cs (34 ticks), frame holds 9 ticks = 98 ms
  token 2..4: '20' -> '38' (2 byte(s) changed)
  [ok  ] own reader parses the edited file (5 lines, 4 names)
  [ok  ] oracle parses the edited file to EOF
  [ok  ] size change equals the token width change (46 -> 46 bytes (+0))
  [ok  ] line count is unchanged (5 line(s))
  [ok  ] every other header token is identical (mode/period/declared/loop)
  [ok  ] the name tokens are identical (4 name(s))
  [ok  ] bytes outside the target token are identical
  [ok  ] own reader sees the new value (38)
  [ok  ] oracle sees the new value (38)
  [ok  ] frame holds the requested delay on the 91 Hz clock (frame 2: 9 ticks = 98 ms)
  [ok  ] oracle agrees on the delay (frame 2: 9 ticks)
  [ok  ] the cycle length covers every frame (34 ticks for 4 frame(s))
  [ok  ] changed-byte count is reported (2 byte(s))
result: OK
```

`--check` против оригинала:

```sh
python3 tools/editor/soawrite.py --check /tmp/opencode/soawrite/Flash3-delay98.soa \
    --against "/mnt/d/games/Gunmetal 3dfx/anims/Flash3.soa" --period 38
```

```
check: /tmp/opencode/soawrite/Flash3-delay98.soa (46 bytes)
  [ok  ] line scanner re-joins the file byte-exactly (5 line(s), final EOL CRLF)
  [ok  ] header is four integers on one line (mode 1, period 38, declared 4, loop 1)
  [ok  ] declared frames within the loader limit (4 (loader: 1..32))
  [ok  ] own header equals the oracle header (1 38 4 1)
  [ok  ] own name tokens equal the oracle names (4 name(s))
  [ok  ] own timeline equals the oracle timeline (4 frame(s), 34 ticks)
  [ok  ] the file is CRLF-only (0 bare LF, 0 bare CR)
  [ok  ] every name line holds one token (4 tokens in 4 line(s))
against: /mnt/d/games/Gunmetal 3dfx/anims/Flash3.soa
  [ok  ] name token counts match the source (4 vs 4)
  [ok  ] line count and line endings match the source (5 vs 5 line(s))
  [ok  ] the file is exactly the source with the field patched (2 byte(s) differ)
  [ok  ] the field carries the expected value (38)
  [ok  ] only the target header token differs (unexpected: none; layout x xx x x -> x xx x x)
field: period = 38, 2 byte(s) differ (2 byte(s) changed)
result: OK
```

Изменены ровно два байта — `cmp -l` (позиции 1-based):

```
$ cmp -l "…/anims/Flash3.soa" /tmp/opencode/soawrite/Flash3-delay98.soa
 3  62  63      # '2' -> '3'
 4  60  70      # '0' -> '8'
```

То есть байты `2..4` файла — ровно спан токена `period`; имя-строки, CRLF и
размер не тронуты.

**Таймлайн до/после.** В поставке кадр 2 держится `9..14` тиков (5 тиков), в
правке — `17..26` (9 тиков):

```sh
python3 gm.py soa "…/anims/Flash3.soa" --timeline
python3 gm.py soa /tmp/opencode/soawrite/Flash3-delay98.soa --timeline
```

```
Flash3.soa       mode=1 period=20 (18 ticks) loop=1 frames=4
      [    0..5    ) 0 flash3a
      [    5..9    ) 1 flash3b
      [    9..14   ) 2 flash3c
      [   14..18   ) 3 flash3d
…
Flash3-delay98.soa mode=1 period=38 (34 ticks) loop=1 frames=4
      [    0..9    ) 0 flash3a
      [    9..17   ) 1 flash3b
      [   17..26   ) 2 flash3c
      [   26..34   ) 3 flash3d
```

`--list` правленого файла показывает ту же картину в мс (задержка кадра 2 =
98 мс, как просили):

```
$ python3 tools/editor/soawrite.py --list /tmp/opencode/soawrite/Flash3-delay98.soa
file: /tmp/opencode/soawrite/Flash3-delay98.soa (46 bytes)
mode: 1 (loop)  period: 38 cs = 34 ticks  declared: 4  loop: 1  names: 4
engine: loadable; layout x xx x x, blank lines 0, padded lines 0, final EOL b'\r\n'
frames: 4 (delay = engine ticks / ms; the format has no per-frame field)
  frame  hold ticks  hold ms   at ms  local id  name
      0           9       98       0         1  flash3a
      1           8       87      98         2  flash3b
      2           9       98     186         3  flash3c
      3           8       87     285         4  flash3d
```

**Читает ли движок.** `.soa` в C++-движке подключён только через `--dump`
(`engine/src/main.cpp:602-622` → `gm::parse_soa`, `engine/src/res/anim.cpp:152`);
рендер уровня `.soa` пока не грузит, поэтому «проверка уровнем» в этом срезе
невозможна (в отличие от `.sob`, где есть `--model --render`). Проверено
чтение парсером движка:

```sh
./build/sandbox/engine/gunmetal --dump "/mnt/d/games/Gunmetal 3dfx/anims/Flash3.soa"
./build/sandbox/engine/gunmetal --dump /tmp/opencode/soawrite/Flash3-delay98.soa
```

```
orig:    kind: SOA animation / mode: 1 period: 20 (18 ticks) loop: 1 / names: 4
edited:  kind: SOA animation / mode: 1 period: 38 (34 ticks) loop: 1 / names: 4
```

Плюс `gm/soa.py` (Python-оракул) видит новый период и новые интервалы
(`--list`/`gm.py soa --timeline` выше). Итог: файл читается и оракулом, и
C++-парсером; поведение в игре не проверялось, потому что движок `.soa` в
рендере не использует.

**Правка с изменением длины** (для проверки splice на другой ширине числа):

```sh
python3 tools/editor/soawrite.py --set Flash3 --period 150 \
    --out /tmp/opencode/soawrite/Flash3-p150.soa
# period: 20 -> 150 (46 -> 47 bytes, delta +1)
#   token 2..4: '20' -> '150' (3 byte(s) changed)
# result: OK
```

`--check` без `--period` в этом случае честно печатает, что файл не
побайтово равен источнику, и какие поля отличаются:

```
  [FAIL] the file is byte-identical to the source (size differs by +1 byte(s))
differs: period 20 -> 150 (a --field expectation would verify this edit)
result: FAIL
```

**Проверка на «грязной» раскладке.** `anims/Conwheel.SOA` — единственный файл,
где имя дополнено пробелами (`conwheea        `), `anims/Brief.SOA` — пустые
строки между именами. Правка периода у обоих сохраняет эти байты: у
`Conwheel` после `--period 40` остаётся `1 40 2 1\r\nconwheea        \r\nconwheeb\r\n`,
у `Brief` имя-часть побайтово равна исходной, размер растёт ровно на ширину
числа (41 → 42).

## 6. `--audit`: что в корпусе (104 файла)

```
audit: 104 files, 8573 bytes, 845 declared frames, 839 name tokens (/mnt/d/games/Gunmetal 3dfx)
  modes: 0=23, 1=77, 100=2, 101=1, 102=1
  frames/file: min 1, median 6, max 32; loadable 102/104 (short 2, extra names 2, special modes 4)
  frames with zero delay: 0 in 0 file(s); min ticks/frame 1.208
  engine refuses (declared > names): anims/copltBLU.soa, anims/copltRED.soa
  extra names the loader never reads (declared < names): anims/watsurf.soa, anims/wrnlit.SOA
  period: 27 distinct, 1..600 cs; top 48=17, 20=9, 72=8, 96=8, 128=7, 60=6, 64=6, 1=4
  size: min 20, median 64.5, max 332 bytes; blank lines 24, padded lines 1, files without final CRLF 1, bare LF/CR 0
  header layouts: 'x xx x x'=56, 'x xx xx x'=23, 'x xxx x x'=11, 'x xxx xx x'=10, 'xxx x x x'=4
  canonical render_lines() identical: 1/104
result: OK
```

Что из этого важно:

* **Нулевых задержек в поставке нет:** минимальное отношение
  `period_ticks / declared` — 1,208 (`pulsar`: 29 тиков на 24 кадра), ни один
  кадр не имеет интервала 0. Инструмент такие кадры строить умеет (self-test:
  `1 2 32 1` даёт нулевые интервалы), но в поставке их нет — значит правка
  `period` не сделает кадр невидимым, пока не уйти ниже `declared` тиков.
* **`loadable` 102/104**: `copltBLU`/`copltRED` объявляют 10 имён при 6
  (`docs/SOA.md` §8) — это свойство данных, `--check` даёт `[warn]`, а
  `--strict` — код 1.
* **4 файла со спецрежимами** (`DCLOCK1`, `dclocksm`, `dclocklg`, `FILM`):
  у них `period = 1` (0 тиков), поэтому `--delay` для них отвергается.
* **27 различных `period`**, мода 48 (17 файлов); размеры 20…332 Б.
* **Канонический писатель не побайтовый** (1/104): unchanged файл пишется
  через сохранённые строки, а после правки сохраняется исходная раскладка,
  потому что меняются только байты токена.

## 7. Ограничения

* **Не создаёт и не удаляет кадры.** `declared` не редактируется, имена кадров
  не редактируются: правка `declared` меняет число читаемых имён (это уже
  работа с данными, а не с токеном), а замена имени меняет ссылку на спрайт в
  BIGFILE. Для этого нужен генератор/редактор имён, а не этот срез.
* **Не меняет имя анимации/id.** Имя — это имя файла (`anims/<name>.soa`), id
  выдаёт глобальный реестр `0x76ed40` в порядке загрузки. `local id` в `--list`
  — порядковый номер первого вхождения имени внутри файла, не id движка.
* **Режимы 100/101/102 не эмулируются.** Инструмент читает и правит их
  заголовок, но «часы»/«фильм» рисует `0x44dc50` (текстура кадра 0), и
  `--delay` для них отвергается: период в этих режимах не используется.
* **Задержка кадра квантуется 91 Гц.** Точное значение ищется перебором
  `period`; невыразимая задержка отвергается (43/54/65… мс), а не округляется.
  Кроме того, все кадры делят один `period`: задать одному кадру задержку, не
  меняя остальные, формат не позволяет — меняются все интервалы.
* **`loop` — мёртвое поле:** правка токена доказуема, но на поведение не
  влияет (`docs/SOA.md` §7).
* **Имена кадров не проверяются по архивам.** `--check` не ищет
  `PICS/SBM.IDX`/`sprites`: отсутствующая картинка — отложенная ошибка движка,
  а не структурная ошибка файла (`docs/SOA.md` §9).
* **C++-движок `.soa` в рендере не использует** — проверка только через
  `--dump`/`parse_soa` и Python-оракул.
* **`--check` без `--against`** делает структурную проверку, но не побайтовое
  сравнение; одноимённый поставочный файл подставляется автоматически только
  если это не тот же самый файл (по относительному пути `anims/…`).

## 8. Проверка

```bash
python3 tools/editor/soawrite.py --self-test      # 68/68
python3 tools/editor/soawrite.py --roundtrip      # 104/104 identical, 8 573 Б
python3 tools/editor/soawrite.py --roundtrip --mirror   # то же на зеркале, 0,01 с
python3 tools/editor/soawrite.py --audit          # числа §6
python3 tools/editor/soawrite.py --list FILE [--limit N]
python3 tools/editor/soawrite.py --check FILE [--against ORIG] [--period CS]
python3 regress.py --mirror --json                # 115/115
```
