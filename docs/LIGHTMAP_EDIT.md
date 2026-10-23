# Редактор лайтмапов `.sbc` (`M11.2`, срез 2)

* **Статус:** срез 2 готов: перечисление записей, экспорт/импорт PNG,
  `--fill`/`--scale`, проверка правки двумя читателями (свой + оракул),
  побайтовый round-trip **51/51** поставочных `.sbc`, self-test **70/70**,
  правка реального `level0.sbc` подтверждена оракулом `gm.sbc`, загрузкой
  (`--info`) и рендером C++-движка (**88** отличающихся пикселей из 64 000).
* **Ветка:** `feat/lightmap`, база `master` `73ea240`, дата 2026-10-23.
* **Файлы:** `tools/editor/lightmapedit.py` (новый), `docs/LIGHTMAP_EDIT.md`
  (этот документ). `gm/*`, `tools/**/*.py` (кроме нового файла), тесты,
  `engine/*`, `gm.py`, `regress.py`, `ROADMAP.md` и прочие существующие файлы
  **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/lightmap`
  (в git не попадают).
* **Оракул:** `gm/sbc.py` (`parse`, `Sbc.to_bytes`, `encode`) — эталон
  раскладки; round-trip в `docs/FORMATS.md` §4 заявлен и здесь измерен
  заново (51/51, 24 214 080 Б).
* **Движок:** `build/sandbox` (Debug, Zig-тулчейн), флаг лайтмапов мира —
  `--lm-world` (в задании он назван `--lightmap-world`; такого флага в CLI
  нет, см. `engine/src/core/config.cpp:298`).

---

## 1. Формат по фактам парсера

`gm/sbc.py` (модульный docstring, `parse`) разбирает блоб ровно до EOF на
всех 51 поставочном файле; загрузчик уровня — `gundx.exe` VMA `0x429a2a`
(чтение), `0x442482` (путь `«<уровень>.SBC»` рядом с `.sbw` и предупреждение
об отсутствии). Файл — плоский контейнер без таблицы смещений:

```
struct Sbc {
    char magic[4];          // "SBC0" или "SBC1"
    u32  count;             // число записей
    count * {
        u32 desc;           // w_log = desc & 0xffff, h_log = desc >> 16
        u8  pixels[];       // (1 << w_log) * (1 << h_log) байт
    };
};
```

Записи хранятся вплотную; `8 + sum(4 + 2**w * 2**h)` равно размеру файла
**точно для всех 51 файла**: 36 в корне установки + 15 в `SOBS/`
(`docs/FORMATS.md` §4 называет разбивку 34 + 17 — общее число то же, 51;
здесь измерено по факту каталогов). Хвостовых байтов нет нигде.

Что важно для редактора (всё — из `gm/sbc.py` и `docs/FORMATS.md` §4):

| факт | значение |
|---|---|
| магия | `SBC1` — 46 файлов, `SBC0` — 5 (все в `SOBS/`) |
| размер записи | `2**w_log x 2**h_log`, обе степени ≤ 16 (предел парсера) |
| тексели | 8-битные, **без палитры**; это готовая яркость |
| `SBC1` | байт на диске = значение, которым движок освещает |
| `SBC0` | на диске 5-битные уровни (измерено: 0..31 в 5 файлах), загрузчик сдвигает `<<2` → 0..124 |
| строки в памяти | шаг **256** байт (на диске — вплотную, `w` байт) |
| ссылка из геометрии | `poly+0x04` (`gm/sbw.Poly.lightmap`), **1-based**, 0 = нет |
| индекс вне файла | загрузчик предупреждает и обнуляет поле (`0x428064`) |
| сумма света | `clamp(poly.brightness + object[0x74] + sector[0x16] + texel, -128, 127)` (`0x43ca5a`..`0x43cc9c`) |

Записи **не самодокументированы по смещению**: размер задаёт `desc`, поэтому
заменить пиксели можно только записью **той же формы** — тогда длина блоба не
меняется и ни один другой байт не сдвигается. Именно это и делает
инструмент; записей он не добавляет и не удаляет.

---

## 2. Что умолчает и что умеет инструмент

| действие | команда | результат |
|---|---|---|
| перечислить записи | `--list LEVEL [--limit N] [--json]` | индекс, размер, тексели, mean/min/max |
| выгрузить запись | `--export LEVEL --record N --png FILE` | 8-битный grayscale PNG (`gm/png.py`) |
| заменить запись из PNG | `--import LEVEL --record N --png FILE [--out FILE]` | те же размеры, иначе отказ |
| залить значением | `--fill LEVEL --record N --value 0..255 [--out FILE]` | все тексели = value |
| масштабировать яркость | `--scale LEVEL --record N --factor F [--out FILE]` | `clamp(round(p*F), 0, 255)` |
| проверить файл | `--check FILE --against LEVEL [--record N --value V \| --png FILE]` | оракул + свой читатель |
| round-trip корпуса | `--roundtrip [--game DIR]` | 51/51 побайтово |
| самотест | `--self-test` | синтетика без файлов игры, 70/70 |

`--out` необязателен: по умолчанию пишется
`/tmp/opencode/lightmap/<level>_r<N>_<op>.sbc`. В каталог установки писать
запрещено — `--out` внутри `--game` отвергается с текстом (код 2), установка
открывается только на чтение.

`--limit` по умолчанию 8, `--limit 0` печатает все записи. `--json` есть у
всех действий и отдаёт те же числа машинно (для `--list --limit 0 --json` —
все 405 записей `level0`).

Коды возврата: `0` — успех; `1` — проверка не прошла (`--check`,
`--roundtrip`, `--self-test`); `2` — неверный ввод или отказ (сообщение в
stderr); `3` — внутренняя ошибка. Трассировка наружу не выходит.

---

## 3. Как инструмент правит и как это доказывается

1. **Свой читатель** (`walk`) проходит блоб с проверкой границ на каждом
   чтении и возвращает для каждой записи смещение `desc`, смещение пикселей,
   `desc`, `w`, `h` и сами байты. `gm.sbc` при этом не используется — это
   независимая реализация той же раскладки, поэтому расхождение читателей
   ловится, а не маскируется.
2. **Сборка** (`serialise`): новый блоб = прежняя магия, прежний `count`,
   **сырые 4 байта `desc`** каждой записи и пиксели; подменяются только
   пиксели целевой записи. Общий размер пересчитывается из записей
   (`8 + sum(4 + texels)`), а не копируется.
3. **Проверка** (`verify_edit`) — 15 обязательных проверок:
   * оракул читает исходный и новый блоб ровно до EOF и **пересобирает
     побайтово** (`Sbc.to_bytes` == файл) — и до, и после правки;
   * число записей и **размеры каждой записи** (`w x h`) не изменились;
   * свой читатель разбирает оба блоба до EOF, магия та же;
   * размер файла не изменился;
   * **все байты вне пикселей целевой записи идентичны** (заголовок, все
     `desc`, все прочие записи);
   * **все прочие записи** совпадают побайтово;
   * изменились только байты пикселей целевой записи (печатается диапазон и
     число изменённых байт);
   * файл действительно изменился (для честного no-op импорта — отдельное
     послабление: если пиксели совпали, результат пишется как есть);
   * свой читатель и оракул видят новые пиксели.

   Файл записывается **только** если все проверки прошли. Иначе — код 1 и
   ничего не пишется.
4. **`--check`** повторяет шаги 1 и 3 на готовом файле: с `--against LEVEL` и
   `--record N` требует «все прочие записи байт в байт, целевая — с новым
   значением» (`--value V` — все тексели равны V, `--png FILE` — пиксели
   совпадают с PNG); без `--record` — полного побайтового равенства
   источнику; без `--against` — структурной проверки (EOF, оракул, размеры).

---

## 4. Правка на реальных данных (`level0.sbc`)

`level0.sbc` — `SBC1`, 405 записей, 51 792 текселя, 53 420 Б; запись 0 —
16x8 (128 текселей), её используют 5 полигонов уровня. Менялась **одна**
запись (`--fill --value 128`); остальные 404 записи и все `desc` не тронуты.

```sh
mkdir -p /tmp/opencode/lightmap
cp "/mnt/d/games/Gunmetal 3dfx/level0.sbt" "/mnt/d/games/Gunmetal 3dfx/level0.sbw" \
   /tmp/opencode/lightmap/
python3 tools/editor/lightmapedit.py --fill level0 --record 0 --value 128 \
    --out /tmp/opencode/lightmap/level0.sbc
```

```
source: /mnt/d/games/Gunmetal 3dfx/level0.sbc (SBC1, 405 records, 53420 bytes)
edit: record[0] 16x8 fill value=128
  mean 70.19 -> 128.00, min 32 -> 128, max 124 -> 128
  [ok] oracle: source round-trip is exact
  [ok] oracle: edited round-trip is exact
  [ok] oracle: record count unchanged
  [ok] oracle: record sizes unchanged
  [ok] own walker: both blobs parse to EOF
  [ok] own walker: magic unchanged
  [ok] own walker: record count unchanged
  [ok] own walker: record sizes unchanged
  [ok] file size unchanged
  [ok] bytes outside the target pixels are identical
  [ok] 404 other records are identical
  [ok] only record[0] pixels differ (span 12..140, 128 byte(s) changed)
  [ok] the file actually changed
  [ok] own walker sees the new pixels
  [ok] oracle sees the new pixels
out: /tmp/opencode/lightmap/level0.sbc (53420 bytes)
result: OK
```

Независимая проверка оракулом (`gm.sbc`, без кода инструмента):

```
magic b'SBC1' b'SBC1' count 405 405 sizes equal True
texels 51792 51792
other records identical: True
record0 new pixels: {128} old mean 70.1875
bytes 53420
roundtrip edited == file: True
```

`--check` на готовом файле:

```sh
python3 tools/editor/lightmapedit.py --check /tmp/opencode/lightmap/level0.sbc \
    --against level0 --record 0 --value 128
```

```
check: /tmp/opencode/lightmap/level0.sbc (53420 bytes)
against: /mnt/d/games/Gunmetal 3dfx/level0.sbc
target: record[0]
  [ok] oracle: source round-trip is exact
  ... (те же 15 проверок правки)
  [ok] oracle sees the new pixels
  differing bytes: 128
result: OK
```

Неверное ожидание отвергается (`--value 127` → `result: FAILED`, код 1).

### 4.1 Движок видит правку

`--info` из каталога с правкой совпадает с оригиналом байт в байт (в том
числе строка лайтмапов), т.е. структура не поехала:

```sh
./build/sandbox/engine/gunmetal -g "/mnt/d/games/Gunmetal 3dfx" -l level0 --info > orig.txt
./build/sandbox/engine/gunmetal -g /tmp/opencode/lightmap      -l level0 --info > edit.txt
diff orig.txt edit.txt        # пусто
```

```
lightmaps: magic=SBC1 records=405 texels=51792
```

Рендер с лайтмапами (`--lm-world`; в каталоге правки — симлинки на `PICS`,
`sprites`, `LOOK16.BMP`, `LOOK16.TAB`, иначе движок не загрузит текстуры):

```sh
./build/sandbox/engine/gunmetal -g "/mnt/d/games/Gunmetal 3dfx" -l level0 \
    --render /tmp/opencode/lightmap/out/orig.png --width 320 --height 200 --lm-world
./build/sandbox/engine/gunmetal -g /tmp/opencode/lightmap -l level0 \
    --render /tmp/opencode/lightmap/out/edit.png --width 320 --height 200 --lm-world
```

```
meshes: 192 faces: 1449
textures: 55 loaded, 0 missing
output: … (256283 bytes)        # оба прогона, код 0
```

Попиксельная сверка (свой разбор PNG через `gm.png.decode`):

```
modes/sizes RGBA 320 200 RGBA 320 200
differing pixels: 88 of 64000
max abs delta: 255
```

88 пикселей — это грани, ссылающиеся на запись 0 (их 5 из 1449), поэтому
правка дошла до растеризатора, а не «проглотилась». Ноль отличий был бы
подозрителен: значит, флаг/путь лайтмапа не сработал.

### 4.2 Экспорт → импорт без изменений

```sh
python3 tools/editor/lightmapedit.py --export level0 --record 0 \
    --png /tmp/opencode/lightmap/out/rec0.png
python3 tools/editor/lightmapedit.py --import level0 --record 0 \
    --png /tmp/opencode/lightmap/out/rec0.png \
    --out /tmp/opencode/lightmap/out/reimport.sbc
cmp "/mnt/d/games/Gunmetal 3dfx/level0.sbc" /tmp/opencode/lightmap/out/reimport.sbc \
    && echo "re-import byte-identical"
```

```
png: L 16 8 bytes 128 min/max 32 124          # экспорт: 115 Б PNG
out: /tmp/opencode/lightmap/out/reimport.sbc (53420 bytes) (identical to the source)
re-import byte-identical
```

То есть PNG — точный носитель записи: `export` → `import` даёт исходный файл
побайтово (`--check` без `--record` на нём тоже зелёный).

### 4.3 `--scale`

```sh
python3 tools/editor/lightmapedit.py --scale level0 --record 0 --factor 0.5 \
    --out /tmp/opencode/lightmap/out/level0_scale.sbc
```

```
edit: record[0] 16x8 scale factor=0.5
  mean 70.19 -> 35.09, min 32 -> 16, max 124 -> 62
  ... (те же 15 проверок)
result: OK
record0 old/new mean: 70.188 35.094
others identical: True
```

### 4.4 `SBC0` (`SOBS/`)

```sh
python3 tools/editor/lightmapedit.py --list "/mnt/d/games/Gunmetal 3dfx/SOBS/HQLift.sbc" --limit 4
```

```
blob: SBC0, 28 records, 1352 texels, 1472 bytes
  rec[   0]   8x4      32 texels  mean=  12.50  min=  8  max= 17
  ...
```

Инструмент работает с `SBC0` так же (round-trip их покрывает), но помнит, что
на диске это 5-битные уровни: движок умножит байт на 4. `--fill 128` в
`SBC0` — это «пересвет» (после `<<2` значение уходит за шкалу и клампится в
сумме света); для `SBC0` осмысленный диапазон правки — 0..31. Инструмент
диапазон не навязывает (поле — u8), но пишет об этом в §7.

---

## 5. Проверки

### 5.1 `--self-test` (без файлов игры), 70/70

Синтетический блоб строится самим тестом (4 записи: 1x1, 2x2, 4x2, 8x8 с
известными пикселями) и проверяется обоими читателями; затем:

* раскладка: магия `+0x00`, `count +0x04`, `desc` записей на 8/13/21,
  первый пиксель на `+0x0c`, `desc` 1x1 = 0, 2x2 = `0x00010001`;
* `build_blob` совпадает с `gm.sbc.encode` (оракул) побайтово;
* PNG: экспорт — mode L нужного размера, пиксели round-trip, импорт
  собственного экспорта — no-op;
* `--fill 128`, `--scale 0.5` (округление `[0,1,2,3] → [0,1,1,2]`),
  `--scale 2.0` (клип 255), `--scale 0.0`, `--scale 1.0` (identity) — каждая
  правка проходит `verify_edit`;
* отказы: запись вне диапазона/отрицательная/без `--record`, `--value`
  256 и −1, `--factor` отрицательный/NaN/∞, PNG другого размера, цветной PNG,
  битый PNG, неверное число байт при сборке (12 отказов);
* обрезка на 4/9/20/последний байт, битая магия, `w_log=17`, лишний
  хвостовой байт — отвергаются и своим читателем, и оракулом;
* проверка не «пустая»: подмена байта в чужой записи и недоделанная правка
  (1 из 4 байт) ловятся;
* полный цикл `export → import → fill → check` через временные файлы,
  включая отказ `--check --record` без `--against`, отказ писать в каталог
  игры и путь по умолчанию под `/tmp/opencode/lightmap`.

```
$ python3 tools/editor/lightmapedit.py --self-test
self-test: 70/70 passed
```

### 5.2 `--roundtrip`

```
$ python3 tools/editor/lightmapedit.py --roundtrip
round-trip: 51/51 identical, 67200 records, 23944872 texels, 24214080 bytes (/mnt/d/games/Gunmetal 3dfx)
magic: SBC0=5, SBC1=46
```

(0.45 с; сравниваются **оба** писателя: свой `serialise` и `Sbc.to_bytes`
оракула — оба побайтово равны файлу.)

### 5.3 Регресс тулкита

```
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 48.3, "full": false, "categories": [],
 "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

---

## 6. Ограничения

* **Новых записей нет.** Число записей (`count`) не меняется: вставить или
  удалить запись нельзя — это сдвинуло бы все последующие и сломало
  `poly+0x04` у геометрии. Для этого нужен писатель уровня/`.sbc`, а не
  редактор пикселей.
* **Размеры записей фиксированы.** `--import` принимает PNG **ровно**
  `2**w_log x 2**h_log`; иначе отказ с текстом. `--fill`/`--scale` форму не
  меняют по определению. `desc` копируется сырым, поэтому даже странное
  слово `desc` не «нормализуется».
* **Палитры нет, только 8-битная яркость.** PNG для импорта обязан быть
  grayscale (mode `L`); цветной/индексированный PNG отвергается, а не
  переводится в серый «на глаз». Значения — байты как они лежат на диске:
  для `SBC1` это и есть шкала движка, для `SBC0` — 5-битные уровни
  (движок умножает на 4), поэтому правки `SBC0` разумно держать в 0..31.
* **Пиксельная правка — не освещение сцены.** Инструмент не знает, какие
  полигоны ссылаются на запись (это `poly+0x04` в `.sbw`, см.
  `tools/editor/sbwedit.py --field lightmap`), не меняет `brightness`
  полигона, `sector+0x16` и не пересчитывает свет. Он меняет только байты
  страницы.
* **`--scale` — линейное масштабирование с клипом** `round(p*F)`; гаммы,
  dithering, размытия и «кисти» нет. Фактор — неотрицательное конечное
  число.
* **Запись не атомарна на уровне ФС**: файл создаётся/перезаписывается
  обычным `open(..., "wb")`; при обрыве возможен недописанный `--out`
  (источник при этом не трогается никогда).
* **Проверка «движок видит»** сделана рендером 320x200 с `--lm-world`
  (88 отличающихся пикселей) и `--info`; полный паритет рендера с
  `gm/render.py` — предмет `docs/RENDER.md`, здесь не пересматривался.
* **Оригинальная игра не запускалась**: проверка — оракул `gm.sbc`, свой
  читатель и наш C++-движок. `gundx.exe` грузит `SBC0` со сдвигом, и это
  поведение здесь не воспроизводилось (см. `docs/LIGHTMAP_AUDIT.md` §1).

---

## 7. Сводка команд

```sh
# что внутри уровня
python3 tools/editor/lightmapedit.py --list level0 --limit 8
python3 tools/editor/lightmapedit.py --list level0 --limit 0 --json

# запись -> PNG -> запись (PNG точный носитель)
python3 tools/editor/lightmapedit.py --export level0 --record 0 --png /tmp/opencode/lightmap/rec0.png
python3 tools/editor/lightmapedit.py --import level0 --record 0 --png /tmp/opencode/lightmap/rec0.png \
    --out /tmp/opencode/lightmap/level0.sbc

# правка без PNG
python3 tools/editor/lightmapedit.py --fill  level0 --record 0 --value 128 \
    --out /tmp/opencode/lightmap/level0.sbc
python3 tools/editor/lightmapedit.py --scale level0 --record 0 --factor 0.5 \
    --out /tmp/opencode/lightmap/level0.sbc

# проверка результата (оракул + свой читатель, все прочие записи)
python3 tools/editor/lightmapedit.py --check /tmp/opencode/lightmap/level0.sbc \
    --against level0 --record 0 --value 128

# корпус и самотест
python3 tools/editor/lightmapedit.py --roundtrip
python3 tools/editor/lightmapedit.py --self-test
```

Рядом: `--game DIR` (по умолчанию `/mnt/d/games/Gunmetal 3dfx`), `--json`,
`--limit N` (`0` — все), `--out PATH`. Просмотр уровня с лайтмапами —
`gm.py view <level>.sbw` (`--no-lightmap`, `--ambient N`), рендер —
`./build/sandbox/engine/gunmetal -l level0 --render out.png --lm-world`.
