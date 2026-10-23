# Редактор геометрии `.sbw` (`M11.2`, срез 1)

* **Статус:** срез 1 готов: перечисление, чтение и правка **документированных
  полей полигона** (шесть полей), побайтовая проверка правки двумя читателями,
  побайтовый round-trip 36/36, self-test 59/59, правка реального
  `level0.sbw` подтверждена оракулом `gm.sbw`, загрузкой и рендером
  C++-движка.
* **Ветка:** `feat/sbwedit`, база `master` `f3a91c7`, дата 2026-10-23.
* **Файлы:** `tools/editor/sbwedit.py` (новый), `docs/SBW_EDIT.md` (этот
  документ). `gm/*`, `tools/editor/*.py` (кроме нового файла), тесты,
  `engine/*`, `gm.py`, `regress.py`, `ROADMAP.md` и прочие существующие файлы
  **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sbwedit`
  (в git не попадают).
* **Оракул:** `gm/sbw.py` (`parse`, `Poly.to_bytes`, `Sbw.to_bytes`) —
  эталон раскладки; round-trip в `docs/FORMATS.md` §10 заявлен и повторно
  измерен здесь (36/36, 40 037 031 Б).

---

## 1. Структура файла по фактам парсера

`gm/sbw.py` (модульный docstring, строки 1-128) разбирает `.sbw` ровно до EOF
на всех 36 поставочных файлах; загрузчик — `gundx.exe` VMA `0x42aa63`
(таблицы), `0x42ad65` (стадия 1), `0x425f4c` (стадия 2). Файл — один
последовательный поток без таблицы смещений:

```
u32 count1;                                  // таблица текстур
count1 * { char name[]; u16 flag; u32 value; }   // value — «вес» уровня: загрузчик
                                                 // кладёт его в 0x5310c0[i] (0x42aba9)
u32 count2;                                  // таблица эффектов/анимаций
count2 * { char name[]; u32 kind; u32 start; u32 n; u16 ids[n]; }
u32 count3;                                  // секторы
count3 * { u32 objects; u16 kind; u8 sub;     // 7 байт, стадия 1
           objects * { u32 flags; u8 header[96]; } }   // 12 x f64: origin + basis 3x3
count3 * objects * {                         // стадия 2, тот же порядок
    u16 n_verts; u16 n_polys;
    f32 verts[n_verts][3];
    f32 radius; u8 f75; u8 f74; u32 f7c; u32 f78;
    f32 bounds[3]; u8 reserved[24];          // 50-байтный хвост объекта
    n_polys * { /* запись полигона */ }
}
```

**Запись полигона на диске — `124 + 2*n_idx` байт**, поля идут не в том
порядке, в каком они лежат в 0x80-байтовой записи движка (`resolve_poly`
`0x434463`; `gm/sbw.py:326-356`):

| смещ. на диске | байт | поле (движковое смещ.) |
|---|---:|---|
| `+0x00` | 2 | `texture` — u16, знаковый (`+0x00`) |
| `+0x02` | 2 | `lightmap` — u16 (`+0x04`) |
| `+0x04` | 2 | `material` — u16 (`+0x02`) |
| `+0x06` | 2 | `overlay` — u16, знаковый (`+0x78`) |
| `+0x08` | 2 | `f7a` (`+0x7a`) |
| `+0x0A` | 1 | `brightness` (`+0x70`) |
| `+0x0B` | 1 | `n_idx` (`+0x71`) |
| `+0x0C` | `2*n_idx` | `indices[]` (`+0x6c`) |
| `+0x0C+2*n` | 112 | хвост: scroll/uv/оси/углы/handle/flags/anim |

В хвосте `flags` лежит по дисковому смещению `+0x66` (то есть
`poly + 0x72 + 2*n_idx`), а не по движковому `+0x7c`; это главная ловушка при
правке «по смещению из FORMATS» — смещения в §10.1 указаны для движковой
записи, а не для файла. Инструмент считает дисковые смещения сам
(`PolyLoc.span`) и проверяет их в self-test (`layout: ...`).

Поэтому любая правка документированного поля — это замена 1/2/4 байт **на
месте**: длина файла не меняется, ни один другой байт не сдвигается, таблицы и
смещения не пересчитываются.

## 2. Доказанные поля

Каждое поле ниже — из `docs/FORMATS.md` §10.1 (там же адреса движка). Ссылка
«док» + VA писателя/читателя; `--fields` печатает эту карту машинно.
Диапазоны дополнительно измерены на всём корпусе: `--audit`, раздел 6.2.

| поле | диск | движок | тип | допустимый диапазон | доказательство |
|---|---|---|---|---|---|
| `texture` | `+0x00` | `+0x00` | s16 | `0` — без текстуры; `1..count1` — 1-based таблица текстур; `-1..-count2` — таблица эффектов | SET_POLY_TEXTURE (id `0x0f`); `gm/sbw.py:75-77,196-202` |
| `lightmap` | `+0x02` | `+0x04` | u16 | `0` — нет; `1..N` — запись sibling-файла `<уровень>.sbc` (N = число записей) | пересчёт базы при загрузке; сэмплер `0x43ca5a`..`0x43cc9c`, `gm/render.py` берёт `records[lightmap-1]` |
| `material` | `+0x04` | `+0x02` | u16 | `0..5` = none/reflective/translucent/varadd/add/shade | таблица `ds:0x5670e0`, шаг `0x40`, имя на `+0x34`; `gm/sbw.py:78-80` |
| `overlay` | `+0x06` | `+0x78` | s16 | та же кодировка, что `texture` | `gm/sbw.py:82,113-114`; в поставке `0`, `-1..-7` и `37..84` |
| `brightness` | `+0x0A` | `+0x70` | u8 | `0..255` (движок складывает и клампит в `-128..127`, `gm/sbc.py` docstring) | SET_POLY_BRIGHTNESS (id `0x10`); `gm/sbw.py:109,178` |
| `flags` | `0x72+2*n_idx` | `+0x7c` | u32 | документированная маска `0x8006`: `0x8000` кадр/UV посчитаны, `0x2` прозрачная, `0x4` переключается командой (`xor [poly+0x7c],4`, `0x46adbc`/`0x46aefa`) | `0x43e02d` пересчитывает UV при сброшенном `0x8000`; `gm/sbw.py:116-119,185` |

Что важно для редактора:

* `texture`/`overlay` хранятся как u16, но **интерпретируются знаково**:
  `0xFFFF` = `-1` = первая запись таблицы эффектов. Оракул отдаёт
  беззнаковое (`gm/sbw.py:330`), инструмент показывает/принимает знаковое и
  пишет two's complement.
* `lightmap` в §10.1 назван знаковым, но в поставке все значения `>= 0`
  (`--audit`: 0..4610) и сэмплер индексирует `records[lightmap-1]`; поэтому
  допустимы `0..N`, отрицательные отвергаются.
* `brightness` документирован как 0..127 (уровень света), но это **u8**, и в
  поставке есть 255 (`0..255`, 148 различных значений) — диапазон поля 0..255;
  как движок трактует >127, в этом срезе не проверялось.
* `flags` — только документированные биты: в поставке ровно
  `{0x8000, 0x8002, 0x8004, 0x8006}` (`regress.py --category sbw` печатает тот
  же набор). Неизвестный бит отвергается, чтобы не записать «мусор».
* Сброс `0x8000` разрешён (это законное состояние: движок пересчитает UV), но
  инструмент печатает предупреждение.

**Поле сектора — не редактируется, и вот почему.** Байт света сектора
существует только в рантайм-записи (in-memory `+0x16`, знаковый;
`docs/FORMATS.md` §10, таблица структуры сектора). В файле заголовок сектора —
это ровно 7 байт (`u32 objects`, `u16 kind`, `u8 sub`), загрузчик пишет их и
инициализирует `+0x16` нулём, а значение задаёт скриптовая команда (таблица
`0x4b6784`, обработчик `0x4578f8`) уже во время игры. На диске патчить нечего,
поэтому `--sector ... --field light` не реализован; `--fields` печатает это
объяснение как `sector.light` в разделе «not editable».

## 3. Как инструмент правит файл

1. **Свой читатель** (`walk`) проходит поток с проверкой границ на каждом
   чтении и возвращает для каждого полигона его дисковое смещение, `n_idx` и
   значения шести полей. `gm.sbw` при этом не используется — это независимая
   реализация той же раскладки, поэтому расхождение читателей ловится, а не
   маскируется.
2. **Проверка значения** (`validate`): диапазон из таблицы §2; для `lightmap`
   число записей берётся из sibling-файла `<уровень>.sbc` (регистронезависимо,
   `lightmap_count`; можно задать `--sbc FILE`). Если `.sbc` нет, правка
   `lightmap` отвергается, а не «угадывается».
3. **Патч** (`patch_bytes`): копия файла, `struct.pack_into` в 1/2/4 байта по
   дисковому смещению. Всё остальное остаётся как было.
4. **Проверка** (`verify_edit`) — 14 проверок, все обязательны:
   * оракул разбирает исходный и новый файл ровно до EOF;
   * совпадают counts (текстуры/эффекты/секторы/объекты/полигоны);
   * **все байты вне полигонов** (таблицы, заголовки секторов и объектов,
     вершины, 50-байтные хвосты) идентичны;
   * **все остальные полигоны** идентичны побайтово;
   * изменились только байты целевого поля (и файл действительно изменился);
   * оба читателя видят новое значение;
   * `Sbw.to_bytes()` оракула побайтово равен исходному и новому файлу.

`--check` повторяет шаги 1 и 4 на готовом файле: с `--against LEVEL` и
`--poly/--field NAME=VALUE` он требует «все прочие полигоны/секторы байт в
байт, целевой — с новым значением»; без цели — полного побайтового равенства
источнику. `--set` всегда пишет в `--out`, в установку игры не пишет никогда.

## 4. CLI

```sh
python3 tools/editor/sbwedit.py --list level0 [--limit N] [--json]
python3 tools/editor/sbwedit.py --get level0 --poly 0 --field texture [--json]
python3 tools/editor/sbwedit.py --set level0 --poly 0 --field texture=28 \
    --out /tmp/opencode/sbwedit/level0.sbw
python3 tools/editor/sbwedit.py --check FILE [--against LEVEL] \
    [--poly N --field NAME=VALUE]
python3 tools/editor/sbwedit.py --roundtrip [--game DIR] [--verbose]
python3 tools/editor/sbwedit.py --self-test
python3 tools/editor/sbwedit.py --audit
python3 tools/editor/sbwedit.py --fields [--json]
```

`--poly N` — глобальный 0-based индекс полигона в порядке `--list`
(сектор → объект → полигон). `--limit 0` печатает всё. `--game` по умолчанию
`/mnt/d/games/Gunmetal 3dfx` (или `GUNMETAL_DIR`).

Коды возврата: `0` — успех; `1` — проверка не прошла (`--check`,
`--roundtrip`, `--self-test`, `--audit`); `2` — неверный ввод или отказ
(сообщение в stderr); `3` — внутренняя ошибка. Трассировка наружу не выходит.

Вывод `--list level0 --limit 4` (сокращённо):

```
level: /mnt/d/games/Gunmetal 3dfx/level0.sbw
header: 254439 bytes, oracle parse ends at 254439 (EOF ok)
tables: textures=381 effects=37
world: sectors=58 objects=192 vertices=2173 polygons=1449
textures (showing 4):
  tex[  1] ALUMNM4          flag=1 value=283
  ...
sectors (showing 4):
  sector[  0] kind= 0 sub=0 objects=1 polys=6
    obj[ 0] id=0x0 verts=8 polys=6 origin=(1432.0, 7568.0, 48.0)
  ...
polygons (showing 4):
  poly[    0] s=  0 o= 0 n_idx= 4 texture=     1 material=0 lightmap=   0 brightness= 64 overlay=   0 flags=0x8000
  ...
```

`--get level0 --poly 0 --field texture`:

```
poly[0] (sector 0, object 0, n_idx 4)
  texture = 1
  disk +0x00 (offset 26345), engine +0x00
  doc: docs/FORMATS.md §10.1 (0x00); gm/sbw.py:75-77,196-202
  engine evidence: SET_POLY_TEXTURE (script id 0x0f)
```

## 5. Правка на реальных данных (`level0.sbw`)

Изменён **один** полигон (`poly 0`, текстура `1 → 28`, обе записи существуют
в таблице уровня; `28` = `WHITE6`). Остальные байты файла не тронуты.

```sh
mkdir -p /tmp/opencode/sbwedit
cp "/mnt/d/games/Gunmetal 3dfx/level0.sbt" "/mnt/d/games/Gunmetal 3dfx/level0.sbc" \
   /tmp/opencode/sbwedit/
python3 tools/editor/sbwedit.py --set level0 --poly 0 --field texture=28 \
    --out /tmp/opencode/sbwedit/level0.sbw
```

```
/mnt/d/games/Gunmetal 3dfx/level0.sbw -> /tmp/opencode/sbwedit/level0.sbw
poly[0] texture: 1 -> 28 (254439 bytes, unchanged size)
  [ok] oracle: source parses to EOF
  [ok] oracle: edited file parses to EOF
  [ok] oracle: texture/effect/sector/object/polygon counts unchanged
  [ok] own walker: both files to EOF
  [ok] own walker: counts unchanged
  [ok] file size unchanged
  [ok] bytes outside polygons are identical
  [ok] 1448 other polygons are identical
  [ok] only texture bytes differ (span 26345..26347, 1 byte(s) changed)
  [ok] the file actually changed
  [ok] own walker sees texture=28
  [ok] oracle sees texture=28
  [ok] oracle round-trip of the source is exact
  [ok] oracle round-trip of the edit is exact
result: OK
```

Проверка оракулом (`gm.sbw`, независимо от инструмента):

```
orig poly0 texture 1 -> 28
tex name WHITE6
differing bytes: 1 [26345]
same size: True parse: True
```

`--check` на готовом файле:

```
python3 tools/editor/sbwedit.py --check /tmp/opencode/sbwedit/level0.sbw \
    --against level0 --poly 0 --field texture=28
```

```
check: /tmp/opencode/sbwedit/level0.sbw (254439 bytes)
against: /mnt/d/games/Gunmetal 3dfx/level0.sbw
target: poly[0] texture=28
  [ok] oracle: parses to EOF
  [ok] own walker: parses to EOF
  [ok] own walker and oracle agree on counts
  [ok] oracle: read -> write is byte-exact
  ... (те же 14 проверок правки)
result: OK
```

C++-движок (`build/sandbox`, Debug) грузит уровень из каталога с правкой; все
counts совпадают с оригиналом:

```
$ ./build/sandbox/engine/gunmetal -g "/mnt/d/games/Gunmetal 3dfx" -l level0 --info
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
lightmaps: magic=SBC1 records=405 texels=51792

$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sbwedit -l level0 --info
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
... (байт в байт те же строки)
```

Рендер завершается кодом 0 и **видит** новую текстуру (в каталоге —
симлинки на `PICS`, `sprites`, `LOOK16.BMP`, поэтому загружаются все 55
текстур; без них движок рисует без текстур и правку не увидеть):

```
$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sbwedit -l level0 \
      --render /tmp/opencode/sbwedit/level0_edit.png --width 320 --height 200
meshes: 192 faces: 1449
textures: 55 loaded, 0 missing
output: /tmp/opencode/sbwedit/level0_edit.png (256283 bytes)     # exit 0

$ sha256sum level0_edit.png level0_orig.png     # оригинал снят с установки
744df8b1…  level0_edit.png
b0b55163…  level0_orig.png
```

Попиксельная сверка (свой разбор PNG, stored-deflate): **75 из 64 000**
пикселей отличаются — правка дошла до растеризатора, а не «проглотилась».
`--info`/рендер при этом не изменили ни одного полигона: 192 объекта,
1449 граней, те же 405 лайтмапов.

## 6. Проверки

### 6.1 `--self-test` (без файлов игры), 59/59

Синтетический `.sbw` строится самим тестом (2 текстуры, 1 эффект, 2 сектора,
3 объекта, 4 полигона) и проверяется обоими читателями; затем:

* раскладка: `texture`/`lightmap`/`material`/`overlay`/`brightness` на
  `+0x00/+0x02/+0x04/+0x06/+0x0A`, `flags` на `0x72+2*n_idx`, размер записи
  `124+2*n_idx`;
* 12 правок (все шесть полей, включая отрицательные `texture`/`overlay`,
  `lightmap` 0 и последнюю запись `.sbc`, `brightness` 127/255, три значения
  `flags`) — каждая проходит все 14 проверок `verify_edit`;
* отказы: `texture` за таблицей и за эффектами, `material=6`, `lightmap`
  сверх числа записей и без `.sbc`, `brightness` 256 и −1,
  `flags=0x8001`, неизвестное поле, не-число, пустая строка, индекс полигона
  вне файла (13 отказов);
* обрезка на 10/60/200 байт и испорченный `n_idx` отвергаются и своим
  читателем, и оракулом;
* проверка не «пустая»: подмена байта вне целевого поля ловится;
* полный цикл `--set` → `--check` через временные файлы, включая отказ без
  `--out` и ожидание неверного значения.

```
$ python3 tools/editor/sbwedit.py --self-test | tail -1
self-test: 59/59 passed
```

### 6.2 `--roundtrip` и `--audit`

```
$ python3 tools/editor/sbwedit.py --roundtrip
round-trip: 36/36 identical, 28192 objects, 235069 polygons, 40037031 bytes
(/mnt/d/games/Gunmetal 3dfx)          # 2.5 s

$ python3 tools/editor/sbwedit.py --audit
audit: 36 files, 235069 polygons (/mnt/d/games/Gunmetal 3dfx)
  texture     shipped range [-13, 213]
  lightmap    shipped range [0, 4610]
  material    shipped range [0, 4]
  overlay     shipped range [-7, 84]
  brightness  shipped range [0, 255]
  flags       shipped range [32768, 32774]
  flags seen: 0x8000, 0x8002, 0x8004, 0x8006
result: OK -- every documented field is inside its range
```

`--audit` — это доказательство диапазонов из §2: каждый полигон каждого
поставочного файла проверен `validate`; `lightmap` — против числа записей
своего `.sbc`. Ноль нарушений на 235 069 полигонах.

### 6.3 Регресс тулкита

```
$ python3 regress.py --mirror --json
{"passed": 114, "total": 114, "seconds": 46.0, "full": false, "categories": [],
 "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

## 7. Ограничения

* **Текстуры и лайтмапы не создаются** — меняются только **индексы** в уже
  существующих таблицах уровня и `.sbc`. Нельзя сослаться на текстуру,
  которой нет в `count1`, или на лайтмап, которого нет в sibling-`.sbc`
  (для этого нужен `bigpack.py`/`sbc`-писатель).
* **Правятся только поля полигона** (§2). Вершины, заголовки объектов
  (origin/basis), UV-оси, `centre`/`normal`/углы, хвост полигона, таблицы
  текстур/эффектов и скрипты — в этом срезе нет. `u_scale`/`v_scale` —
  производные (`uv_fp >> 7`), их правка не имеет смысла без правки UV.
* **Сектор не редактируется**: его единственное «поле» (`light`, +0x16) —
  рантайм-состояние, на диске его нет (§2).
* **Полигоны не добавляются/не удаляются** и `n_idx` не меняется: запись
  самоделимитирующаяся, но вставка сдвинула бы всё после неё — это отдельная
  задача (писатель уровня, а не редактор поля).
* `flags` принимает только документированную маску `0x8006`; сброс `0x8000`
  разрешён (движок пересчитает UV), но помечается предупреждением.
* `brightness > 127`: поле u8 и такие значения есть в поставке, но как движок
  их использует (кладёт в `bright + obj[0x74] + level[0x16]` и клампит), в этом
  срезе не проверялось; редактор их принимает как валидные байты.
* Проверка «движок видит правку» сделана рендером 320×200 (75 отличающихся
  пикселей) и `--info`; полноценный паритет рендера с `gm/render.py` — предмет
  `docs/RENDER.md`, здесь не пересматривался.
* `--check` без `--against` сверяет только структуру (EOF, counts, round-trip);
  побайтовое сравнение с источником включается `--against LEVEL` (или
  автоматически, если рядом с `--game` есть файл с тем же именем).

## 8. Сводка команд

```sh
# что внутри уровня
python3 tools/editor/sbwedit.py --list level0 --limit 8
python3 tools/editor/sbwedit.py --list level0 --limit 0 --json

# карта полей с provenance (док + VA)
python3 tools/editor/sbwedit.py --fields

# чтение/правка одного документированного поля
python3 tools/editor/sbwedit.py --get level0 --poly 0 --field lightmap
python3 tools/editor/sbwedit.py --set level0 --poly 0 --field brightness=127 \
    --out /tmp/opencode/sbwedit/level0.sbw

# проверка результата (оракул + свой читатель, все прочие байты)
python3 tools/editor/sbwedit.py --check /tmp/opencode/sbwedit/level0.sbw \
    --against level0 --poly 0 --field brightness=127

# корпус, диапазоны, самотест
python3 tools/editor/sbwedit.py --roundtrip
python3 tools/editor/sbwedit.py --audit
python3 tools/editor/sbwedit.py --self-test
```
