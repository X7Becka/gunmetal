# Запись `.sof`: побайтовый round-trip и правка пикселя (`M11.5`, срез 7)

* **Статус:** срез готов. Round-trip **48/48** файлов поставки побайтово
  (95 330 304 Б, 11 637 кадров), собственный писатель и писатель оракула
  совпадают **48/48**, файл, записанный на диск и прочитанный обратно,
  побайтово равен исходнику **48/48**. Правка одного пикселя
  `Film/AMMO4.SOF` (кадр 0, `x=47, y=39`, индекс `0 → 253`) меняет **ровно
  1 байт** из 1 081 344, `--check` (с ожиданием пикселя и без) ок, C++
  `--film` и `gm/sof.py` дают одинаковый FNV-1a64 кадра до и после. Self-test
  **49/49**.
* **Ветка:** `feat/sofwrit`, база `master` `efa0ff6`, дата 2026-10-24.
* **Файлы:** `tools/editor/sofwrit.py` (новый), `docs/SOF_WRITE.md` (этот
  документ). Существующие файлы (`gm/sof.py`, `gm/*.py`, `gm.py`,
  `engine/*`, `render/hud.*`, `res/sof.*`, `tools/re/*`,
  `tools/editor/*`, `regress.py`, `ROADMAP.md`, `docs/*.md`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sofwrit`
  (в git не попадают).
* **Оракул:** `gm/sof.py` (`parse`, `decode_tile`, `Sof.frame`,
  `Sof.to_bytes`, `load_palette`); раскладка — `docs/FORMATS.md` §11.1,
  транспонирование и блит — `engine/src/res/sof.h`, `docs/HUD_CPP.md` §1,
  `docs/FILM.md`; палитра — `gm/palette.py`.

> **Число файлов.** В поставке v1.36 ровно **48** `.sof`, все в `Film/`
> (рекурсивный обход корня игры других каталогов не нашёл). Все размеры
> делятся на 8192 нацело: 95 330 304 = 11 637 × 8192, поэтому «хвостового»
> неполного тайла в поставке нет ни у одного файла (писатель его всё равно
> сохраняет — см. §7).

---

## 1. Раскладка и почему правка байтовая

`.sof` — **без заголовка**: плоский поток тайлов по 8192 байта. Тайл — один
кадр 128×64 индексов палитры, но хранится **транспонированным**: 64 столбца
× 128 строк (`docs/FORMATS.md` §11.1). Читатель `0x49fd40` делает
`offset = frame << 13` и `fread(buf, 1, 0x2000)`, блиттер `0x49fe48` —
`src[x*64 + y] -> dst[y*128 + x]`.

Отсюда единственная формула, на которой стоит вся правка
(`tools/editor/sofwrit.py`, `pixel_offset`):

```
offset(frame, x, y) = frame * 8192 + x * 64 + y      (x = 0..127, y = 0..63)
```

Доказательство — в коде декодера: `decode_tile` копирует
`tile[x*64 : (x+1)*64]` в позиции `x, x+128, x+256, …` кадра, то есть
`decoded[y*128 + x] = tile[x*64 + y]`. Ни заголовка, ни таблицы кадров, ни
per-frame метаданных в файле нет, поэтому «правка» — это подмена ровно
одного байта в тайле; размер файла не меняется, остальные 8191 пиксель кадра
и все остальные кадры остаются теми же байтами.

Тот же факт в C++: `engine/src/res/sof.h` держит `data` целиком
(`to_bytes()` = копия `data`), а `decode_sof_tile` — то же транспонирование,
так что байтовая правка не требует ни пересборки, ни пересчёта.

### Почему статистику кадра можно считать на хранимом тайле

Транспонирование — биекция множества байтов тайла на множество байтов кадра.
Поэтому **минимум, максимум, число уникальных индексов и «все байты равны»**
не зависят от того, применён ли к тайлу декодер. `--audit`/`--list` считают
эти числа на хранимом тайле (это в 100 раз дешевле декодирования 11 637
кадров), а self-test отдельно проверяет, что `sorted(tile) == sorted(decoded)`
и что `frame_stats` даёт те же числа для синтетического кадра.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/sofwrit.py --roundtrip
```

```
48/48 identical, 95330304 bytes, 11637 frame(s), 0.90 s (/mnt/d/games/Gunmetal 3dfx)
  own reader: 48/48 parsed; own writer re-emits 48/48; oracle writer 48/48
  written to disk and read back byte-exactly: 48/48
result: OK
```

Каждый файл: читается своим разборщиком (границы тайлов + хвост), пишется
обратно **своим** писателем и писателем оракула, затем пишется в scratch-каталог,
читается с диска и сравнивается с оригиналом побайтово.

| метрика | результат |
|---|---|
| найдено `.sof` (рекурсивно от корня игры) | 48 (все в `Film/`) |
| **побайтово идентичны после read → write → read** | **48 / 48** (95 330 304 Б) |
| свой разборщик доходит до конца | 48 / 48 |
| свой писатель (`to_bytes`) воспроизводит файл | 48 / 48 |
| писатель оракула (`gm/sof.py`) воспроизводит файл | 48 / 48 |
| запись на диск + чтение обратно побайтово | 48 / 48 |
| файлов с неполным хвостовым тайлом | 0 |
| время | ≈0,9 с на `/mnt/d`, ≈0,25 с на зеркале (`--mirror`) |

Формат тривиален настолько, что «round-trip» здесь — это проверка границ, а
не кодера: писатель обязан вернуть те же 8192-байтовые блоки в том же порядке
и не потерять хвост. Ошибка в границах кадра проявилась бы сразу: `--audit`
показал бы лишний/недостающий кадр, а `--check` — расхождение с оракулом.

## 3. `--audit`: что в корпусе (48 файлов)

```sh
python3 tools/editor/sofwrit.py --audit
```

```
audit: 48 files, 95330304 bytes, 11637 frame(s) of 8192 bytes (/mnt/d/games/Gunmetal 3dfx)
  frames/file: min 30, median 132, max 1755; unaligned sizes: 0 file(s)
  palette indices: 0..254 corpus-wide
  unique colours/file: min 32, median 116, max 254; corpus-wide 255 of 256
  empty frames (a single palette index): 55 in 4 file(s)
    BIOFORT.SOF (1: 106)
    COMMS.SOF (9: 107, 1747, 1748, 1749, 1750, 1751…)
    CTHU.SOF (8: 383, 384, 385, 386, 387, 388…)
    Default.sof (37: 0, 1, 2, …)
  largest: COMMS.SOF, 1755 frames, 14376960 bytes, 254 colour(s)
result: OK
```

| признак | значение |
|---|---|
| файлов / байт / кадров | 48 / 95 330 304 / 11 637 |
| размер кадра | 8192 Б (128×64 индексов) — у всех, хвостов нет |
| кадров в файле | min 30 (`Kelfilm.sof`), медиана 132, max 1755 (`COMMS.SOF`) |
| размер файла | min 245 760 Б (30 кадров), max 14 376 960 Б (1755) |
| индексы палитры в корпусе | **0..254** (индекс 255 не используется нигде) |
| уникальных цветов в файле | min 32, медиана 116, max 254 |
| уникальных цветов в корпусе | 255 из 256 |
| «пустых» кадров (один индекс) | **55** в 4 файлах |

Пустые кадры — это данные, а не ошибка раскладки: `Default.sof` (заставка
«по умолчанию», 290 тайлов) держит 37 полностью одноцветных кадров
(`0..27` в начале и `281..289` в конце), `CTHU.SOF` — 8 подряд (`383..390`),
`COMMS.SOF` — 9 (`107` и `1747..1754`), `BIOFORT.SOF` — один (`106`). Ни один
из них не является неполным тайлом: размер всегда 8192 Б.

Индекс 255 отсутствует во всём корпусе, хотя байт под него есть: правка
`--index 255` допустима формально (это валидный индекс VGA-палитры), но в
поставке такого пикселя нет — инструмент не выдумывает «запрещённые» значения.

## 4. Правка пикселя на реальных данных: `Film/AMMO4.SOF`

Файл: 132 кадра, 1 081 344 Б; кадр 0 — 56 уникальных цветов, 8187 пикселей с
ненулевым индексом и 5 «дырок» (индекс 0), например `(47, 39)`.

```sh
python3 tools/editor/sofwrit.py --set-pixel \
    "/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF" \
    --frame 0 --x 47 --y 39 --index 253 \
    --out /tmp/opencode/sofwrit/demo/AMMO4-px.sof
```

```
/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF -> /tmp/opencode/sofwrit/demo/AMMO4-px.sof
frame 0 of 132, pixel (x=47, y=39): 0 -> 253
  file offset 3047 (tile offset 3047, tile byte [x*64 + y])
  [ok  ] exactly the requested bytes differ from the source (1 byte changed (0x00 -> 0xfd))
  [ok  ] file size is unchanged (1081344 bytes)
  [ok  ] the differing byte is pixel (x=47, y=39) of frame 0 (offset 3047 (tile offset 3047))
  [ok  ] own decoder reads the new index at that pixel (253)
  [ok  ] only that one pixel of the frame changed (1 pixel(s))
  [ok  ] every other frame is byte-identical (131 frame(s))
  [ok  ] the edited film keeps the tail verbatim (0 tail byte(s))
  [ok  ] the oracle sees the new index at that pixel (253 (gm/sof.py))
bytes changed: 1 of 1081344, size 1081344
result: OK
```

Смещение считается по формуле §1: `0*8192 + 47*64 + 39 = 3047`.

`--check` против оригинала (с ожиданием правки и без него):

```sh
python3 tools/editor/sofwrit.py --check /tmp/opencode/sofwrit/demo/AMMO4-px.sof \
    --against "/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF" \
    --frame 0 --x 47 --y 39 --index 253
```

```
check: /tmp/opencode/sofwrit/demo/AMMO4-px.sof (1081344 bytes)
frames: 132, frame bytes 8192, tail 0, indices 0..253, colours 113
  [ok  ] own reader parses the file (132 frame(s), 0 tail byte(s))
  [ok  ] the size is a whole number of 8192-byte frames (1081344 = 132 * 8192)
  [ok  ] the oracle re-emits the file byte-exactly (gm/sof.py) (132 tile(s))
  [ok  ] own decoder agrees with the oracle on every frame (132 frame(s))
  [ok  ] palette indices fit one byte (0..253 in this film, 113 unique colour(s))
against: /mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF
  [ok  ] size matches the source (1081344 vs 1081344 bytes)
  [ok  ] one byte differs from the source (as --set-pixel writes) (1 byte at offset 3047 (frame 0, pixel x=47 y=39))
  [ok  ] the source and the file differ in exactly one pixel (1 differing byte(s))
  [ok  ] the changed byte is the requested pixel (frame 0, x=47, y=39) (offset 3047, expected 3047)
  [ok  ] the requested pixel carries the expected index (253, expected 253)
changed byte: offset 3047 = frame 0, pixel (x=47, y=39), 0 -> 253
result: OK
```

Изменён ровно один байт — `cmp -l` (позиции 1-based):

```
$ cmp -l "/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF" \
         /tmp/opencode/sofwrit/demo/AMMO4-px.sof
   3048   0 375      # 0x00 -> 0xfd = 253
```

То есть байт с нулевым смещением 3047 — ровно `tile[47*64 + 39]` кадра 0;
размер, остальные кадры и порядок тайлов не тронуты.

**Весь корпус.** Та же операция (первый пиксель `(0, 0)` кадра 0, индекс,
отличный от текущего) прогнана по всем 48 файлам: **48/48** дают ровно один
изменённый байт, и `--check` против исходника везде ок. Смещение у всех одно —
`0*8192 + 0*64 + 0 = 0`, то есть первый байт любого `.sof` и есть пиксель
`(0, 0)` кадра 0 (обратная проверка формулы §1).

**Кто это видит.** Python-оракул (`gm/sof.py`) и C++-читатель (`--film`,
`gm::parse_sof`) дают одинаковый FNV-1a64 декодированного кадра 0 до и после:

| файл | `gm/sof.py` | `gunmetal --film` |
|---|---|---|
| `AMMO4.SOF` (поставка) | `06531011db474c67` | `06531011db474c67` |
| `AMMO4-px.sof` (правка) | `c09f8b372426105c` | `c09f8b372426105c` |

```sh
./build/sandbox/engine/gunmetal --film AMMO4 --game "/mnt/d/games/Gunmetal 3dfx" --frame 0
./build/sandbox/engine/gunmetal --film AMMO4-px.sof --game /tmp/opencode/sofwrit/demo --frame 0
```

Одинаковые хэши означают, что C++ декодирует транспонирование так же, как
формула §1, и правка не «съехала» по координатам. Поведение в самой игре не
проверялось (инструмент оффлайн; правка меняет только индекс пикселя).

## 5. `--png`: диагностика правки

Экспорт кадра — не обязательный пункт, а способ увидеть правку глазами:

```sh
python3 tools/editor/sofwrit.py --png \
    "/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF" --frame 0 \
    --out /tmp/opencode/sofwrit/demo/before.png
python3 tools/editor/sofwrit.py --png /tmp/opencode/sofwrit/demo/AMMO4-px.sof \
    --frame 0 --out /tmp/opencode/sofwrit/demo/after.png
```

```
$ … --png "/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF" --frame 0 \
      --out /tmp/opencode/sofwrit/demo/before.png
/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF (frame 0 of 132) -> /tmp/opencode/sofwrit/demo/before.png
png: 128x64 indexed, 1956 bytes; palette from gm.palette.installed(/mnt/d/games/Gunmetal 3dfx)
pixels: 8187 with a non-zero index, 5 at index 0, 56 unique colour(s)
result: OK

$ … --png /tmp/opencode/sofwrit/demo/AMMO4-px.sof --frame 0 \
      --out /tmp/opencode/sofwrit/demo/after.png
/tmp/opencode/sofwrit/demo/AMMO4-px.sof (frame 0 of 132) -> /tmp/opencode/sofwrit/demo/after.png
png: 128x64 indexed, 1956 bytes; palette from gm.palette.installed(/mnt/d/games/Gunmetal 3dfx)
pixels: 8188 with a non-zero index, 4 at index 0, 56 unique colour(s)
result: OK
```

Число непустых пикселей выросло ровно на 1 (8187 → 8188), число цветов не
изменилось (253 уже был в кадре), длина PNG совпала (1956 Б в обоих случаях,
хотя байты разные: `cmp -l` даёт 782 расхождения — `gm/png.py` сжимает zlib,
и одна замена пикселя не изменила длину). PNG пишется 8-битным
**индексированным** (colour type 3), поэтому `gm/png.py decode` возвращает ту
же матрицу индексов и ту же палитру — self-test это проверяет.

## 6. CLI

```sh
python3 tools/editor/sofwrit.py --roundtrip [DIR] [--mirror] [--json]
python3 tools/editor/sofwrit.py --audit [DIR] [--mirror] [--json]
python3 tools/editor/sofwrit.py --list FILE [--limit N] [--json]
python3 tools/editor/sofwrit.py --check FILE [--against ORIG]
                                       [--frame N --x X --y Y --index I]
                                       [--json]
python3 tools/editor/sofwrit.py --set-pixel FILE --frame N --x X --y Y \
                                       --index I --out OUT [--json]
python3 tools/editor/sofwrit.py --png FILE --frame N --out OUT.png [--json]
python3 tools/editor/sofwrit.py --self-test [--json]
```

`FILE` — путь или имя (`AMMO4`, `AMMO4.SOF`, регистр не важен: ищется в
`Film/`, затем рекурсивно по установке). `--game` по умолчанию
`/mnt/d/games/Gunmetal 3dfx` (или `$GUNMETAL`). `--roundtrip`/`--audit` без
аргумента обходят корень игры рекурсивно; явный `DIR` заменяет его,
`--mirror` подставляет зеркало.

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод/отказ
(сообщение в stderr), `3` внутренняя ошибка. Трассировка наружу не выходит.

Отказы (проверены self-test'ом и вручную):

```
$ … --set-pixel "$S" --frame 0 --x 0 --y 0 --index 256 --out /tmp/…/x.sof
sofwrit: --index 256 is outside 0..255 (one palette index is one byte)
# exit 2 — то же для -1

$ … --set-pixel "$S" --frame 132 --x 0 --y 0 --index 1 --out /tmp/…/x.sof
sofwrit: frame 132 is outside 0..131 (132 frame(s))
# exit 2 — то же для отрицательного кадра, x=128, y=64

$ … --set-pixel "$S" --frame 0 --x 0 --y 0 --index 1 \
      --out "/mnt/d/games/Gunmetal 3dfx/Film/X.SOF"
sofwrit: refusing to write inside the game installation: … (write to /tmp/opencode or /var/tmp)
# exit 2 — то же для зеркала (~/.cache/gunmetal-re/game) и репозитория

$ … --list "/mnt/d/games/Gunmetal 3dfx/Film/weapon.fdx"
sofwrit: not a .sof file: …/weapon.fdx
# exit 2 — вход/выход обязан быть .sof (.png для --png)

$ … --set-pixel FILE --frame 0 --x 0 --y 0 --index 1 --out FILE
sofwrit: --out must differ from the input file: FILE          # exit 2
```

Дополнительно отвергаются: пустой файл, размер больше `MAX_TILES * 8192`
(1 048 576 кадров), неполный хвостовой тайл у `--set-pixel`/`--png`
(`--audit`/`--list`/`--check` его показывают и `--check` возвращает 1),
палитра короче 256 квадов у `--png`, `--check --frame/--x/--y/--index` не
полным набором. Запись атомарная (`mkstemp` в каталоге цели + `fsync` +
`os.replace`).

`--set-pixel` не пишет файл, если хоть одна из 8 проверок не прошла: сначала
все доказательства, потом `os.replace`. Значение, равное текущему, допустимо:
пишется копия, `changed = 0` и это честно печатается.

## 7. Ограничения

* **Не меняет число кадров и размер кадра.** Кадр — всегда 8192 Б, фильм —
  целое число кадров; вставка/удаление кадра сдвинула бы все последующие
  смещения и потребовала бы правки `.fdx` (`count`), поэтому это работа
  другого инструмента. `--set-pixel` и `--png` отказываются от файла с
  неполным хвостовым тайлом, а не «дополняют» его.
* **Не трогает `.fdx`.** Правка пикселя не влияет на `(count, fps)`; связка
  `WEAPON8.SOF` ↔ строка 8 `weapon.fdx` и расхождение `count` (131) с числом
  тайлов (132) — свойство данных (`docs/FORMATS.md` §11.1), инструмент его не
  «исправляет».
* **Палитры в `.sof` нет.** Индекс — просто байт; `--set-pixel` принимает
  любое значение 0..255 и не проверяет, «осмысленно» ли оно для общей
  VGA-таблицы. `--png` берёт таблицу из `gm.palette.installed()` (движковый
  `FRONTEND/GAMEPAL` из архива), с откатом на `gm/sof.load_palette()`
  (`Film/Kflm0000.bmp`); это диагностика, а не доказательство палитры фильмов.
* **Не перекодирует тайл.** Правка идёт в хранимый транспонированный байт;
  декодировать кадр, менять матрицу и кодировать обратно инструмент умеет
  (кодек есть и проверен), но `--set-pixel` этого не делает намеренно: любой
  такой путь — это лишний шанс изменить 8192 байта вместо одного.
* **Не двигает кадры и не меняет их порядок.** Секвенции задаёт `.fdx`/скрипт
  `FILM_DEFINE`, а не файл.
* **Проверка «в игре» не делалась.** Доказательства: побайтовый round-trip
  48/48, совпадение с оракулом и C++-читателем по FNV кадра, 1 изменённый
  байт по `cmp`. Как выглядит правленый кадр в HUD — вопрос запуска
  оригинала, а не формата.

## 8. Проверка

```bash
python3 tools/editor/sofwrit.py --self-test           # 49/49
python3 tools/editor/sofwrit.py --roundtrip           # 48/48 identical, 95 330 304 Б
python3 tools/editor/sofwrit.py --roundtrip --mirror  # то же на зеркале, 0,25 с
python3 tools/editor/sofwrit.py --audit               # числа §3
python3 tools/editor/sofwrit.py --list FILE [--limit N]
python3 tools/editor/sofwrit.py --check FILE [--against ORIG]
python3 regress.py --mirror --json                    # 115/115, 60,6 с
```
