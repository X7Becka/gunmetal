# Запись `.sob`: байт-в-байт round-trip и правка токенов (`M11.5`, срез 2)

* **Статус:** срез готов: побайтовый round-trip **672/672** моделей поставки
  (4 671 847 Б), карта токенов восстанавливает файл **672/672**
  (789 439 токенов), правка меняет **только байты целевого токена** (2 байта
  на примере `vertex.0.z`), self-test **67/67**, правка `TICK.sob`
  подтверждена загрузкой и рендером C++-движка (`bounds` сдвинулся ровно на
  правку, 10 059 пикселей кадра изменились).
* **Ветка:** `feat/sobwrite`, база `master` `b61c2f2`, дата 2026-10-24.
* **Файлы:** `tools/editor/sobwrite.py` (новый), `docs/SOB_WRITE.md` (этот
  документ). Существующие файлы (`gm/*.py`, `tools/re/sob_writer.py`,
  `tools/editor/*.py`, `gm.py`, `regress.py`, `ROADMAP.md`, `docs/*.md`,
  тесты, `engine/*`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sobwrite`
  (в git не попадают).
* **Оракул:** `gm/sob.py` (`parse`, `Sob.to_bytes`, `Sob.write`), формат —
  `docs/FORMATS.md` §3, раскладка и варианты — `docs/SOB.md` §1–§2.

> **Число моделей.** В задании среза названо 663 модели; фактически в
> поставочной v1.36 **672** файла `.sob`, и все они лежат в `SOBS/`
> (рекурсивный обход корня игры: `SOBS/` — 672, больше нигде `.sob` нет).
> Все числа ниже — по факту обхода, не по числу из задания.

---

## 1. Раскладка и почему правка байтовая

`.sob` — плоский поток токенов, разделённых пробельными символами; переводы
строк для парсера не значимы (`gm/sob.py:1-30`, `docs/FORMATS.md` §3):

```
<vertex_count> <face_count>
vertex_count * (x y z)                        // float, мировые единицы
face_count * {
    8 токенов-имён                            // [0] диффузная текстура,
                                              // [1..7] флаги материала
    flags nverts i0 .. i(nverts-1)            // целые; 3 <= nverts <= 32
    15 чисел                                  // origin(3) U(3) V(3) N(3) spare(3)
    [ -1 ]                                    // необязательный одиночный -1
}
```

Каждое поле — **один токен** (вектор — три соседних токена). Канонический
писатель оракула (`Sob.to_bytes`) печатает вариант A и поэтому **не**
побайтовый: из 672 поставочных файлов с ним совпадают 77, остальные
отличаются раскладкой (`docs/SOB.md` §2.2):

| вариант | признак | файлов |
|---|---|---:|
| A | `-1` нет, после 15 чисел CRLF | 77 |
| B | `-1` в конце целочисленной строки, после 15 чисел нет CRLF | 436 |
| B + финальный CRLF | то же + перевод строки в конце файла | 50 |
| «длинные» float | `%.15G`-подобная орфография (`1.22124532708767E-15`) | 96 |
| прочее | `MOCLASS.SOB` (однопробельная раскладка) и иная орфография `E`-чисел | 13 |

Побайтовое совпадение неизменённой модели даёт `raw` (писатель возвращает
исходные байты), но **правка** через `to_bytes` переписала бы весь файл в
вариант A: у 595 файлов поменялась бы раскладка, у 96 — орфография чисел.
Поэтому инструмент правит **только байты целевого токена**:

1. `tokenize` режет файл так же, как `str.split()` оракула (latin1: один байт =
   один символ), и запоминает `(start, end)` каждого токена;
2. свой `walk` (независимая реализация грамматики) запоминает для каждой
   вершины и каждой грани её токены и их байтовые диапазоны; необязательный
   одиночный `-1` ищется откатом с мемоизацией по позиции токена, как в
   `gm/sob.py` и в движке (`engine/src/res/sob.cpp`), поэтому вариант B
   читается так же (`origin.x == -1`);
3. `splice` собирает файл из исходных байтов, подменяя только диапазоны
   целевых токенов. Разделители (пробелы, CRLF, выравнивание `"% d "`) не
   трогаются; длина файла меняется только на разницу ширины нового числа
   (`1432` → `1496` — 0, `12` → `1.5` — +1, `18` → `-1` — −1).

## 2. Что доказано round-trip'ом

`python3 tools/editor/sobwrite.py --roundtrip` (≈5 с на `/mnt/d`: 5,4 с в замере):
каждая модель читается, пишется оракулом в scratch-каталог
(`Sob.write(exact=True)`), читается с диска обратно и сравнивается с
оригиналом.

```
672/672 identical, 4671847 bytes (/mnt/d/games/Gunmetal 3dfx)
  own walker: 672/672 parsed to EOF; token-span re-join: 672/672 (789439 tokens)
  semantic re-parse (parse -> to_bytes -> parse): 672/672
  canonical layout identical to the shipped file: 77/672
result: OK
```

| метрика | результат |
|---|---|
| файлов найдено (рекурсивно от корня игры) | 672 |
| **побайтово идентичны после read → write → read** | **672 / 672** (4 671 847 Б) |
| свой walker доходит до EOF | 672 / 672 |
| карта токенов восстанавливает файл (`rejoin`) | 672 / 672, 789 439 токенов |
| семантический round-trip (`parse → to_bytes → parse`) | 672 / 672 |
| канонический текст совпал с файлом | 77 / 672 |
| вершин / граней | 31 090 / 24 036 |

`rejoin` — отдельное доказательство, что байтовые диапазоны токенов полны и
не пересекаются: файл собирается из `data[..token.start] + token.text + …` и
обязан совпасть побайтово. Это то, на чём стоит байтовая правка.

## 3. Как инструмент правит файл

1. `walk` строит карту токенов и проверяет границы на каждом шаге: счётчики
   (`MAX_VERTS` 200 000 / `MAX_FACES` 5 000 — те же, что у оракула), `3 *
   nverts` вершин, у грани 8 текстур, `flags nverts + nverts индексов`
   (`3..32`, индексы `0 <= i < vertex_count`), 15 конечных чисел; хвостовые
   токены — ошибка (`ScanError`, код 2).
2. `resolve_field` превращает имя поля в конкретные токены (`FieldSpec`);
   `parse_assignment` разбирает значение, `validate` сверяет с диапазоном,
   измеренным на корпусе.
3. `splice` подменяет только эти токены; `changed_bytes` заново разбирает
   результат и считает, сколько байт реально изменилось (выравнивание от
   начала токена + разница длины).
4. `verify_edit` (9 проверок, все обязательны):
   * свой walker и оракул разбирают новый файл **до EOF**;
   * размер изменился ровно на разницу ширины токенов, число токенов то же;
   * **все токены вне цели идентичны** (и, отдельно, все байты вне диапазонов
     цели — включая разделители — идентичны);
   * новый walker и оракул видят записанное значение;
   * число изменённых байт посчитано и напечатано.

Вершины/полигоны не создаются и не удаляются: `nverts`/`nfaces`/индексы не
редактируются (их текст сдвинул бы поток). Нормали, радиус, UV и `spare` не
пересчитываются.

## 4. CLI

```sh
python3 tools/editor/sobwrite.py --list TICK.sob [--limit N] [--json]
python3 tools/editor/sobwrite.py --get TICK.sob --field face.0.flags [--json]
python3 tools/editor/sobwrite.py --set TICK.sob --field vertex.0.z=60 \
    --out /tmp/opencode/sobwrite/TICK-shift.sob
python3 tools/editor/sobwrite.py --check FILE [--against ORIG] \
    [--field NAME=VALUE] [--json]
python3 tools/editor/sobwrite.py --roundtrip [DIR] [--json]
python3 tools/editor/sobwrite.py --audit [DIR] [--json]
python3 tools/editor/sobwrite.py --self-test [--json]
```

`FILE` — путь или имя модели (`TICK` ищется как `SOBS/TICK.sob`, регистр не
важен). `--game` по умолчанию `/mnt/d/games/Gunmetal 3dfx` (или `$GUNMETAL`).
`--roundtrip`/`--audit` без аргумента обходят каталог игры рекурсивно; явный
`DIR` (позиционный) заменяет его.

Редактируемые поля:

| поле | токенов | смысл |
|---|---:|---|
| `vertex.N` | 3 | мировая вершина `x,y,z` |
| `vertex.N.x` / `.y` / `.z` | 1 | одна компонента |
| `face.N.origin` | 3 | origin плоскости (первые 3 из 15) |
| `face.N.u` / `.v` | 3 | оси проекции (16.16, масштаб 65536) |
| `face.N.normal` | 3 | N-вектор (масштаб не документирован) |
| `face.N.spare` | 3 | маркер `(0, 12345678\|16539982\|…, 0)`, недокументирован |
| `face.N.flags` | 1 | целое; в поставке −128…127 (96 значений) |
| `face.N.texture` | 1 | диффузная текстура (слот 0) |
| `face.N.slotK`, K=0..7 | 1 | любой текстовый слот (`none`, `translucent`, `add`, …) |

Диапазоны отказа (измерены `--audit`): `|vertex| <= 1e9` (в поставке максимум
13 779,594), `|число плоскости| <= 1e12` (максимум 1 073 610 747,5), `flags`
−128…127, текстовый токен — печатный ASCII без пробелов (latin-1).

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод/отказ
(сообщение в stderr), `3` внутренняя ошибка. Трассировка наружу не выходит.

Отказы (проверены self-test'ом и вручную):

```
$ python3 tools/editor/sobwrite.py --set TICK.sob --field vertex.0.z=1 \
      --out "/mnt/d/games/Gunmetal 3dfx/SOBS/TICK.sob"
sobwrite: refusing to write inside the game installation: …
# exit 2

$ python3 tools/editor/sobwrite.py --list /tmp/opencode/nothing.txt
sobwrite: model not found: /tmp/opencode/nothing.txt (looked in …)
# exit 2

$ python3 tools/editor/sobwrite.py --set TICK.sob --field vertex.99.z=1 \
      --out /tmp/opencode/sobwrite/x.sob
sobwrite: vertex.99: the model has 62 vertices (0..61)          # exit 2

$ … --set TICK.sob --field face.0.flags=999 --out /tmp/…
sobwrite: face.0.flags=999: face flags are a byte read by int() in the engine;
shipped range -128..127 (96 distinct values)                    # exit 2
```

`--set` также отказывает, если `--out` совпадает с входным файлом (правка
на месте запрещена). Запись атомарная (`mkstemp` в каталоге цели + `fsync` +
`os.replace`, `gm/sob.py:atomic_write`).

## 5. Правка на реальных данных: `TICK.sob`, вершина 0

```sh
mkdir -p /tmp/opencode/sobwrite
python3 tools/editor/sobwrite.py --set TICK.sob --field vertex.0.z=60 \
    --out /tmp/opencode/sobwrite/TICK-shift.sob
```

```
/mnt/d/games/Gunmetal 3dfx/SOBS/TICK.sob -> /tmp/opencode/sobwrite/TICK-shift.sob
vertex.0.z: 18 -> 60.0 (7564 -> 7564 bytes, delta +0)
  token 21..23: '18' -> '60' (2 byte(s) changed)
  [ok  ] own walker parses the edited file
  [ok  ] oracle parses the edited file to EOF
  [ok  ] size change equals the token width change (7564 -> 7564 bytes (+0))
  [ok  ] token count is unchanged (1439 tokens)
  [ok  ] every other token is identical (0 unexpected change(s))
  [ok  ] bytes outside the target token(s) are identical
  [ok  ] own walker sees the new value (60)
  [ok  ] oracle sees the new value (60)
  [ok  ] changed-byte count is reported (2 byte(s))
result: OK
```

`--check` против оригинала:

```sh
python3 tools/editor/sobwrite.py --check /tmp/opencode/sobwrite/TICK-shift.sob \
    --against "/mnt/d/games/Gunmetal 3dfx/SOBS/TICK.sob" --field vertex.0.z=60
```

```
against: /mnt/d/games/Gunmetal 3dfx/SOBS/TICK.sob
  [ok  ] token counts match the source (1439 vs 1439)
  [ok  ] vertex/face counts match the source
  [ok  ] the file is exactly the source with the field patched (2 byte(s) differ, 1 token(s) differ)
  [ok  ] the field carries the expected value (60)
  [ok  ] only the target token(s) differ (0 unexpected change(s))
field: vertex.0.z = 60, 2 byte(s) differ, 1 token(s) differ
result: OK
```

Изменены ровно два байта (`1`→`6`, `8`→`0`); все остальные 7 562 байта —
те же, раскладка варианта B сохранена.

**Проверка движком** (`--model` читает `.sob` напрямую,
`engine/src/main.cpp:1001-1013`):

```sh
./build/sandbox/engine/gunmetal --game "/mnt/d/games/Gunmetal 3dfx" \
    --model "…/SOBS/TICK.sob" --render /tmp/opencode/sobwrite/tick-orig.png \
    --width 320 --height 200
./build/sandbox/engine/gunmetal --game "/mnt/d/games/Gunmetal 3dfx" \
    --model /tmp/opencode/sobwrite/TICK-shift.sob \
    --render /tmp/opencode/sobwrite/tick-shift.png --width 320 --height 200
```

```
orig:   bounds: (-28, -48, -17) .. (28, 65, 18)   meshes: 1 faces: 43
edited: bounds: (-28, -48, -17) .. (28, 65, 60)   meshes: 1 faces: 43
sha256 orig   ffc7bd1b8dbbc0d607c92dba3457b2814bb0bbe15b66cd27c6e21bd44554a960
sha256 edited 7c4d968efb80794a000cdbfef69623447dfb1baaedcacb9f0b49b4452f9bc1be
```

Движок сам печатает AABB по загруженным вершинам: максимум по Z вырос ровно
на правку (18 → 60). Попиксельная сверка двух кадров (`gm/png.py`,
320×200): **10 059 пикселей** отличаются, отличия в прямоугольнике
`x 83..242, y 58..164`; «непустых» пикселей (не фон-градиент строки) было
**8 461**, стало **6 923** — силуэт изменился. То есть файл читается не только
парсером, но и движком.

## 6. `--audit`: что в корпусе (672 модели)

```
audit: 672 models, 4671847 bytes, 31090 vertices, 24036 faces
  vertices/model: min 4, median 34, max 957; faces/model: min 1, median 27, max 1130
  vertices/model buckets: 0-16: 203, 17-64: 300, 65-256: 166, 257-1024: 3, 1025+: 0
  faces/model buckets: 0-16: 242, 17-64: 336, 65-256: 92, 257-1024: 1, 1025+: 1
  faces by vertex count: 3: 4817, 4: 18089, 5: 150, 6: 778, 7: 12, 8: 173, 9: 4, 10: 2, 16: 11
  textures: 526 distinct; material tokens: none=153999, 0=10834, add=2726, translucent=343, varadd=34, 1=13, 2=12, 3=10
  flags: 96 distinct, range -128..127; top 64=7308, 48=1884, 0=1585, 127=1559, 88=1396, 8=1270
  spare triples: 11 distinct; top (0,16539982,0)=10960, (0,12345678,0)=10749, (0,0,0)=1190, (0,12345934,0)=982, (0,16540494,0)=123, (0,16540750,0)=14
  plane axes (U/V/N, 72108 triples): |a|=65536 19186, zero 8974, other 43948
  lone -1 after the plane numbers: 0 faces; origin.x=-1 (variant B alignment): 21357 faces
  models with no material token: 316
  lightmaps: field in .sob = False (0 faces) -- lightmaps live in .sbw/.sbc
  canonical layout identical: 77/672; semantic re-parse: 672/672
```

Что из этого важно:

* **Лайтмапов в `.sob` нет** ни у одной из 24 036 граней: 8 текстовых слотов,
  `flags/nverts/индексы` и 15 чисел плоскости полностью объясняют грамматику.
  Индекс лайтмапа — поле полигона `.sbw` (`docs/FORMATS.md` §10.1), страницы —
  `<уровень>.sbc` (`docs/FORMATS.md` §4). Поэтому `--audit` печатает
  `field_in_sob: false`; это измерение, а не предположение.
* **Неизвестные/недокументированные числовые поля:** `flags` (96 значений,
  −128…127; семантика в доках не описана), `spare` (11 троек; чаще всего
  `(0, 16539982, 0)` и `(0, 12345678, 0)`), необязательный `-1` после 15 чисел
  (0 граней в поставке) и 7 «материальных» слотов (коды `none`, `add`,
  `translucent`, `varadd`, цифры `0..7`). Инструмент переносит их как есть.
* **Оси не всегда `65536`:** только 19 186 из 72 108 троек U/V/N имеют
  норму ровно 65536, 8 974 — нулевые, 43 948 — прочие. Формулировка «U/V/N —
  единичные оси, умноженные на 65536» верна как правило, но не как инвариант
  (то же видно по `normal`, где в поставке встречаются значения до
  1,07 млрд). Это факт данных; нормали не пересчитываются.
* `origin.x == -1` у 21 357 граней — это вариант B, где `-1` в конце
  целочисленной строки читается как первое число плоскости (совпадает с
  движком и `gm/sob.py`), а не отдельный флаг.

## 7. Ограничения

* **Не создаёт и не удаляет вершины/полигоны.** `vertex_count`,
  `face_count`, `nverts` и индексы не редактируются: их текст сдвинул бы
  поток, а проверка «изменились только байты цели» перестала бы быть
  локальной. Для добавления геометрии нужен генератор, а не этот срез.
* **Нет пересчёта производных данных:** нормали, `origin/u/v`, `spare`,
  радиус/box (их в `.sob` нет), UV-проекция не пересчитываются; правка
  вершины не двигает плоскости граней. Это осознанно: доказывать новую
  цепочку пересчёта нечем.
* **Нет `scale`/массовых трансформаций:** равномерный масштаб изменил бы
  ширину каждого числа и сдвинул весь файл; такой правки в срезе нет.
* **Текстуры и лайтмапы не трогаются:** правка `face.N.texture`/`slotK` меняет
  только имя-токен; файлы `PICS/SBM.IDX`, `.sbc` и сами BMP остаются как были
  (и писать в каталог игры инструмент отказывается).
* **Канонический писатель не побайтовый** (77/672): неизменённая модель
  пишется из `raw`, а после правки сохраняется исходная раскладка, потому что
  меняются только байты токена. Если правка меняет ширину числа, длина файла
  меняется на эту разницу — сама раскладка при этом не перестраивается.
* **`--check` без `--against`** делает структурную проверку (включая
  семантический round-trip и точную запись оракула), но не побайтовое
  сравнение: сравнивать не с чем. Одноимённый поставочный файл подставляется
  автоматически, только если это не тот же самый файл.
* **Границы значений** (`|vertex| <= 1e9`, `|плоскость| <= 1e12`,
  `flags` −128…127) — измеренные по поставке; значения вне них отвергаются
  текстом, а не пишутся «вслепую».
* Числа пишутся кратчайшей точной формой (`12`, `1.5`, `26214.400391`), как
  `gm/sob.py`; `NaN`/`inf` отвергаются.

## 8. Проверка

```bash
python3 tools/editor/sobwrite.py --self-test        # 67/67
python3 tools/editor/sobwrite.py --roundtrip        # 672/672 identical, 4 671 847 Б
python3 tools/editor/sobwrite.py --audit            # числа §6
python3 tools/editor/sobwrite.py --check FILE [--against ORIG] [--field N=V]
python3 regress.py --mirror --json
```
