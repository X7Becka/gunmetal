# Объекты `.sbw`: редактор (`M11.2`, срез 3)

* **Статус:** срез 3 готов: перечисление объектов, чтение/правка
  `origin`/`radius`/`bounds`/`f74`/`f75`/`f78`/`f7c`, побайтовая проверка
  правки двумя читателями, round-trip **36/36**, self-test **82/82**, правка
  `level0.sbw` подтверждена оракулом `gm.sbw`, загрузкой и рендером
  C++-движка.
* **Ветка:** `feat/sbwobj`, база `master` `ae3882d`, дата 2026-10-23.
* **Файлы:** `tools/editor/sbwobj.py` (новый), `docs/SBW_OBJECTS.md` (этот
  документ). Существующие файлы (`gm/*.py`, `tools/**/*.py`, `docs/*.md`,
  тесты, `engine/*`, `gm.py`, `regress.py`, `ROADMAP.md`) **не менялись**.
* **Соседний срез:** поля полигонов — `tools/editor/sbwedit.py`,
  `docs/SBW_EDIT.md`; здесь правятся **объекты**, а не полигоны.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sbwobj*`
  (в git не попадают).
* **Оракул:** `gm/sbw.py` (`parse`, `Object.points`,
  `Object.world_vertices`, `Sbw.to_bytes`) — эталон раскладки.

---

## 1. Где лежит объект

Объект присутствует в файле **дважды** (`gm/sbw.py:1-128`,
`docs/FORMATS.md` §10, `docs/WORLD_FIELDS.md` §2.3/§3):

```
стадия 1 (трансформ, все секторы и объекты подряд):
    u32 id;                 // в gm/sbw.py — Object.id; в рантайме obj+0x70
                            // это БИТОВОЕ ПОЛЕ (см. §2), а не «номер»
    u8  header[96];         // 12 x f64: origin[3] + basis[3][3] (row-major)

стадия 2 (геометрия, тот же порядок сектор -> объект):
    u16 n_verts; u16 n_polys;
    f32 verts[n_verts][3];  // локальные координаты
    f32 radius;             // +0x60: радиус описанной сферы
    u8  f75;                // +0x75
    u8  f74;                // +0x74
    u32 f7c;                // +0x7c
    u32 f78;                // +0x78
    f32 bounds[3];          // +0x84: полуразмеры AABB
    u8  reserved[24];       // +0x90: 24 нуля (3 x f64 «extent»)
    n_polys * { /* запись полигона, 124 + 2*n_idx байт */ }
```

Мировые координаты: `world = origin + basis · local` (`Object.world_vertices`,
трансформ `0x437af8` читает заголовок как `FLD double`, вершину как
`FLD float`). `origin` и `basis` — **f64**, `radius`/`bounds` — **f32**.

Абсолютные смещения на примере `level0.sbw`, объект 0 (`--get` печатает их
для любого объекта):

| что | диск | размер |
|---|---|---:|
| `id`-слово | `6596` (`header+0x00`) | 4 |
| `origin` | `6600` (`header+0x04`) | 24 |
| `basis` | `6624` (`header+0x1C`) | 72 |
| `n_verts`/`n_polys` | `26195` (`geom+0x00`) | 4 |
| вершины | `26199` (`geom+0x04`) | `12*n_verts` |
| `radius` | `26295` (`tail+0x00`) | 4 |
| `f75` / `f74` | `26299` / `26300` (учтено: `tail+0x04`/`+0x05`) | 1 + 1 |
| `f7c` / `f78` | `26301` / `26305` (`tail+0x06`/`+0x0A`) | 4 + 4 |
| `bounds` | `26309` (`tail+0x0E`) | 12 |
| `reserved` | `26321..26345` (`tail+0x1A`) | 24 |
| первый полигон | `26345` | `124 + 2*n_idx` |

Порядок полей в 50-байтном хвосте и в полигоне разный по отношению к
движковым смещениям (`+0x60`/`+0x74`/`+0x75`/`+0x78`/`+0x7c`/`+0x84`/`+0x90`):
инструмент считает **дисковые** смещения сам (`ObjectLoc.span`) и проверяет
их в self-test (`layout: ...`).

---

## 2. Что доказано

Каждое поле — из `docs/FORMATS.md` §10 и `docs/WORLD_FIELDS.md` §2.3/§3 (там
же адреса). `--fields` печатает эту карту машинно; `--audit` измеряет
диапазоны на всех 36 файлах.

| поле | диск | движок | тип | что доказано | допустимо инструментом |
|---|---|---|---:|---|---|
| `origin` | `header+0x04` | `+0x00` | 3 × f64 | загрузчик копирует 24 Б без конверсии (`0x42ae86`), трансформ `0x437af8` (`FLD double`/`FMUL`/`FADD`); AABB сектора (`0x434fed`) содержит вершины у 4716/5104 секторов | `origin=x,y,z`, конечные f64, `|v| ≤ 1e12` |
| `basis` | `header+0x1C` | `+0x18` | 9 × f64 | строки `0x42aea7/0x42aec8/0x42aee9`; норма каждой строки ровно 1.0 у **84 576/84 576**; единичный базис у **28 192/28 192** объектов | только `basis=identity` |
| `radius` | `tail+0x00` | `+0x60` | f32 | загрузчик `0x425f4c`; cull `0x437d8c`/`0x437d05`; сфера сектора `0x4360f1` | неотрицательный конечный f32 |
| `bounds` | `tail+0x0E` | `+0x84` | 3 × f32 | загрузчик `0x425f4c`; пересчёт `0x43529f` (FABS компонент), AABB сектора `0x434fed` | 3 неотрицательных конечных f32 (полуразмеры) |
| `f75` | `tail+0x04` | `+0x75` | u8 | загрузчик `0x425f4c`; в поставке `{0, 1}`; читателя не найдено | 0..255 |
| `f74` | `tail+0x05` | `+0x74` | u8 | обнуляется `0x42ae55`, читается как i8 в `0x43c51f`/`0x43d8e3` (поправка яркости объекта, `docs/WORLD_MATERIALS.md` §3.5); в поставке 0 | 0..255 |
| `f7c` | `tail+0x06` | `+0x7c` | u32 | загрузчик; в поставке 0; читателя не найдено | 0..255 (см. ограничения) |
| `f78` | `tail+0x0A` | `+0x78` | u32 | загрузчик; в поставке 0; читателя не найдено | 0..255 (см. ограничения) |
| `id`-слово | `header+0x00` | `+0x70` | u32 | **не id, а битовое поле**: `0x10` cull луча, `0x10000` трассы, `0x20000` форс-рендер, `0x40000/0x80000` «массивы не принадлежат объекту», `0x100000` не пересчитывать UV (`docs/WORLD_MATERIALS.md` §4.1) | **не правится**, только показывается |
| `reserved[24]` | `tail+0x1A` | `+0x90` | 3 × f64 `extent` | загрузка; читают `0x434fed`/`0x437d05`/`0x440867`; 0 во **всех** 28 192 объектах, писателей в коде мира нет | **не правится**, сохраняется дословно |

**Почему `radius` — f32, а не f64.** В `gm/sbw.py` (`<f` по смещению `tail+0`,
строки 449-451) и в `docs/FORMATS.md` §10 (`f32 radius`) поле занимает 4 байта;
восемь байт здесь перекрыли бы `f75`/`f74`. Запись 8 байт сдвинула бы весь
хвост и была бы отвергнута проверкой «only radius bytes differ».

**Почему `basis` только identity.** В поставке базис единичный у всех 28 192
объектов, поэтому объект — чистый перенос. Произвольный поворот/масштаб
потребовал бы пересчёта кэша кадра каждого полигона (`normal`/`u_axis`/
`v_axis`/`centre`/`corner_a`/`corner_b` — они авторские и лежат в
объект-локальном кадре; `0x43529f` пересчитывает только `bounds` и углы),
радиуса/`bounds` и AABB сектора. Ни поставочного примера, ни теста движка для
этой цепочки нет, поэтому такое значение **отвергается текстом**, а не
пишется «вслепую»:

```
$ python3 tools/editor/sbwobj.py --set level0 --object 0 \
      --field basis=2,0,0,0,1,0,0,0,1 --out /tmp/opencode/sbwobj/x.sbw
sbwobj: basis='2,0,0,0,1,0,0,0,1': only basis=identity is accepted: the basis is
the identity in 28192/28192 shipped objects (docs/FORMATS.md §10,
docs/WORLD_FIELDS.md §3), while an arbitrary rotation/scale would invalidate the
per-polygon frame cache (normal/u_axis/v_axis/centre/corners are authored in the
object-local frame and 0x43529f only recomputes bounds/corners), the object
radius/bounds and the sector AABB (0x434fed); no shipped example or engine test
proves that chain, so the edit is refused instead of written blind
# exit 2
```

`basis=identity` на поставочных данных — легальный no-op: файл пишется
байт-в-байт, проверка это фиксирует (`no-op: the file already carries
basis=identity...`), код возврата `0`.

---

## 3. Как инструмент правит файл

1. **Свой читатель** (`walk`) проходит поток с проверкой границ на каждом
   чтении и запоминает для каждого объекта: диск-смещение id-слова и
   заголовка, `n_verts`/`n_polys`, смещение вершин и 50-байтного хвоста,
   конец полигонов, а также значения всех документированных полей.
   `gm.sbw` при этом не используется — это независимая реализация той же
   раскладки.
2. **Проверка значения** (`validate`): форма (три числа для `origin`/`bounds`),
   конечность, знак (`radius`/`bounds` неотрицательны), диапазон
   (`f74/f75/f78/f7c` — 0..255), `basis` — только `identity`.
3. **Патч** (`patch_bytes`): `struct.pack_into` ровно в байты поля. Длина файла
   не меняется, ничего не сдвигается, вершины и полигоны не пересобираются.
4. **Проверка** (`verify_edit`, 18 проверок, все обязательны):
   * оракул разбирает исходный и новый файл ровно до EOF;
   * counts (текстуры/эффекты/секторы/объекты/полигоны) и id-слова объектов
     не изменились;
   * **все байты вне целевого объекта** (таблицы, заголовки секторов, чужие
     заголовки/вершины/хвосты/полигоны) идентичны;
   * **все остальные объекты** (заголовок + стадия 2) идентичны побайтово;
   * **локальные вершины целевого объекта не изменились**;
   * изменились только байты целевого поля (или файл не изменился вовсе -- это
     легальный no-op, когда значение уже совпадало);
   * оба читателя видят новое значение;
   * `Sbw.to_bytes()` оракула побайтово равен исходному и новому файлу;
   * **мировые вершины:** для `origin` — сдвинулись ровно на дельту `origin`
     (допуск `1e-6`, фактически 0), для остальных полей — не изменились;
     ни один другой объект в мировом кадре не сдвинулся.

Проверка «мировые вершины» — это и есть содержательное доказательство, что
`origin` двигает объект **целиком**: локальные вершины те же, мир сдвинут на
дельту, прочие объекты на месте.

`--check FILE --against LEVEL [--object N --field NAME=VALUE]` повторяет те же
проверки на готовом файле; без `--field` требует полного побайтового равенства
источнику. `--set` всегда пишет в `--out` и отказывается писать внутрь
каталога игры (`--game`), даже если такой `--out` передан явно.

---

## 4. CLI

```sh
python3 tools/editor/sbwobj.py --list level0 [--limit N] [--json]
python3 tools/editor/sbwobj.py --get level0 --object 0 --field origin [--json]
python3 tools/editor/sbwobj.py --set level0 --object 0 --field origin=1496,7568,48 \
    --out /tmp/opencode/sbwobj/level0.sbw
python3 tools/editor/sbwobj.py --check FILE [--against LEVEL] \
    [--object N --field NAME=VALUE]
python3 tools/editor/sbwobj.py --roundtrip [--game DIR] [--verbose]
python3 tools/editor/sbwobj.py --self-test
python3 tools/editor/sbwobj.py --audit
python3 tools/editor/sbwobj.py --fields [--json]
```

`--object N` — глобальный 0-based индекс в порядке `--list` (сектор →
объект), тот же порядок, что у `gm/sbw.py`. `--limit 0` печатает всё.
`--game` по умолчанию `/mnt/d/games/Gunmetal 3dfx`.

Коды возврата: `0` — успех; `1` — проверка не прошла (`--check`,
`--roundtrip`, `--self-test`, `--audit`); `2` — неверный ввод или отказ
(сообщение в stderr); `3` — внутренняя ошибка. Трассировка наружу не выходит.

`--list level0 --limit 4` (сокращённо, блок `sectors` опущен):

```
level: /mnt/d/games/Gunmetal 3dfx/level0.sbw
header: 254439 bytes, oracle parse ends at 254439 (EOF ok)
tables: textures=381 effects=37 (end 5320, 6585)
world: sectors=58 objects=192 vertices=2173 polygons=1449
objects (showing 4):
  obj[    0] sector=  0 id=0x00000000 verts=   8 polys=   6 radius=289.993
    origin=(1432, 7568, 48) bounds=(184, 184, 128) f74=0 f75=0 f7c=0 f78=0
    basis=identity
    aabb=((1248, 7384, -80) .. (1616, 7752, 176))
  obj[    1] sector=  1 id=0x00000000 verts=   8 polys=   6 radius=62.9921
    origin=(16, -296, 56) bounds=(48, 8, 40) f74=0 f75=0 f7c=0 f78=0
    basis=identity
    aabb=((-32, -304, 16) .. (64, -288, 96))
  ...
```

`--get level0 --object 0 --field origin`:

```
obj[0] (sector 0, index 0, id word 0x00000000, 8 verts, 6 polys)
  origin = (1432, 7568, 48)
  disk header+0x04 (offset 6600), 24 bytes, engine +0x00
  doc: docs/FORMATS.md §10 (Трансформ объекта); gm/sbw.py:389-391
  engine evidence: loader copies 24 raw bytes at 0x42ae86; transform 0x437af8 (FLD double)
  aabb = ((1248, 7384, -80) .. (1616, 7752, 176))
```

`--get level0 --object 0 --field radius` (видно и расхождение с `|bounds|`):

```
obj[0] (sector 0, index 0, id word 0x00000000, 8 verts, 6 polys)
  radius = 289.99310302734375
  disk tail+0x00 (offset 26295), 4 bytes, engine +0x60
  doc: docs/FORMATS.md §10; gm/sbw.py:80,449-451; docs/WORLD_FIELDS.md §2.3
  engine evidence: loader 0x425f4c; cull 0x437d8c/0x437d05; sector sphere 0x4360f1
  |bounds| = 289.993 (difference 3.39e-07)
  aabb = ((1248, 7384, -80) .. (1616, 7752, 176))
```

`--get level0 --object 0 --field basis`:

```
obj[0] (sector 0, index 0, id word 0x00000000, 8 verts, 6 polys)
  basis = identity
  disk header+0x1C (offset 6624), 72 bytes, engine +0x18
  doc: docs/FORMATS.md §10; gm/sbw.py:389-391; docs/WORLD_FIELDS.md §2.3
  engine evidence: loader 0x42aea7/0x42aec8/0x42aee9 (3 x 24 B); transform 0x437af8
  note: basis is the identity, so world = origin + local
  aabb = ((1248, 7384, -80) .. (1616, 7752, 176))
```

---

## 5. Правка на реальных данных (`level0.sbw`)

### 5.1 `origin` объекта 0: +64 по X

Объект 0 — один из «дальних» по X (его AABB `x = 1248..1616` задаёт границу
уровня). Сдвиг на `+64` виден и в AABB уровня, который печатает сам движок.

```sh
mkdir -p /tmp/opencode/sbwobj
cp "/mnt/d/games/Gunmetal 3dfx/level0.sbt" "/mnt/d/games/Gunmetal 3dfx/level0.sbc" \
   /tmp/opencode/sbwobj/
for f in PICS sprites LOOK16.BMP; do
  ln -sfn "/mnt/d/games/Gunmetal 3dfx/$f" "/tmp/opencode/sbwobj/$f"
done
python3 tools/editor/sbwobj.py --set level0 --object 0 \
    --field origin=1496,7568,48 --out /tmp/opencode/sbwobj/level0.sbw
```

```
/mnt/d/games/Gunmetal 3dfx/level0.sbw -> /tmp/opencode/sbwobj/level0.sbw
obj[0] origin: (1432, 7568, 48) -> (1496, 7568, 48) (254439 bytes, unchanged size)
  [ok] oracle: source parses to EOF
  [ok] oracle: edited file parses to EOF
  [ok] oracle: texture/effect/sector/object/polygon counts unchanged
  [ok] oracle: object id words unchanged
  [ok] own walker: both files to EOF
  [ok] own walker: counts (objects/vertices/polygons) unchanged
  [ok] file size unchanged
  [ok] bytes outside the target object are identical
  [ok] 191 other objects (header + vertices + tail + polygons) are identical
  [ok] local vertices of the target object are unchanged
  [ok] only origin bytes differ (span 6600..6624, 1 byte(s) changed)
  [ok] the file actually changed
  [ok] own walker sees origin=(1496, 7568, 48)
  [ok] oracle sees origin=(1496, 7568, 48)
  [ok] oracle round-trip of the source is exact
  [ok] oracle round-trip of the edit is exact
  [ok] no other object moved in the world frame (world = origin + basis * local)
  [ok] world vertices of the target object shifted exactly by (64, 0, 0) (8 vertices, max deviation 0)
  note: object translated as a whole: 8 local vertices untouched, world frame shifted by (64, 0, 0)
result: OK
```

`--check` на готовом файле (те же 18 проверок + структура):

```sh
python3 tools/editor/sbwobj.py --check /tmp/opencode/sbwobj/level0.sbw \
    --against level0 --object 0 --field origin=1496,7568,48
```

```
check: /tmp/opencode/sbwobj/level0.sbw (254439 bytes)
against: /mnt/d/games/Gunmetal 3dfx/level0.sbw
target: obj[0] origin=(1496, 7568, 48)
  [ok] oracle: parses to EOF
  [ok] own walker: parses to EOF
  [ok] own walker and oracle agree on counts
  [ok] oracle: read -> write is byte-exact
  ... (далее те же 18 проверок правки)
result: OK
```

Сверка **независимым** скриптом на `gm.sbw` (без кода инструмента):

```
same size: True parse EOF: True True
counts equal: True
obj0 origin (1432.0, 7568.0, 48.0) -> (1496.0, 7568.0, 48.0)
obj0 radius 289.99310302734375 -> 289.99310302734375
differing bytes: [6606]
origin delta: (64.0, 0.0, 0.0)
objects whose world vertices moved: [0]
max deviation from the delta: 0.0
obj0 world[0]: (1616.0, 7752.0, 176.0) -> (1680.0, 7752.0, 176.0)
obj0 local vertices unchanged: True
obj0 radius unchanged: True
```

То есть изменился **один байт** (старший байт мантиссы `1432.0 -> 1496.0`),
локальные вершины и все прочие объекты — те же, мир сдвинут ровно на `+64`
по X и ни на что больше.

C++-движок (Debug-сборка **этого worktree** `build/sandbox` от `ae3882d`:
`cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug … && cmake
--build build/sandbox -j`, бинарь `build/sandbox/engine/gunmetal`) грузит
уровень из каталога с правкой; counts совпадают с оригиналом **байт в байт**:

```
$ ./build/sandbox/engine/gunmetal -g "/mnt/d/games/Gunmetal 3dfx" -l level0 --info
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
lightmaps: magic=SBC1 records=405 texels=51792

$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sbwobj_full_origin -l level0 --info
... (те же строки дословно, exit 0)
```

Движок сам печатает AABB уровня по загруженным вершинам — и он сдвинулся
ровно на `+64` по X, то есть правку видит не только наш парсер, но и движок:

```
orig:   bounds: (-3080, -408, -1920) .. (1616, 8836, 288)
edited: bounds: (-3080, -408, -1920) .. (1680, 8836, 288)
```

Рендер 320×200 (`--render`, 55 текстур, 192 меша, 1449 граней) отличается:

```
$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sbwobj_full_orig   -l level0 \
      --render .../frame.png --width 320 --height 200   # exit 0
$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sbwobj_full_origin -l level0 \
      --render .../frame.png --width 320 --height 200   # exit 0
sha256: b0b55163…  frame.png   (оригинал)
        f21fe46a…  frame.png   (origin +64)
```

Попиксельная сверка (`gm/png.py`): **750 из 64 000** пикселей отличаются
(1,17 %), отличия лежат в прямоугольнике `x 84..254, y 71..118` — то есть
ровно там, куда переехал объект. `--info` при этом не изменился: 192 объекта,
1449 граней, те же 405 лайтмапов.

> Для рендера каталог правки — это **вся** установка, слинкованная по файлам,
> с реальным `level0.sbw` (`/tmp/opencode/sbwobj_full_origin`):
>
> ```sh
> for d in /tmp/opencode/sbwobj_full_orig /tmp/opencode/sbwobj_full_origin; do
>   mkdir -p "$d"
>   for f in "/mnt/d/games/Gunmetal 3dfx"/*; do
>     [ "$(basename "$f")" = level0.sbw ] || ln -sfn "$f" "$d/"
>   done
>   cp "/mnt/d/games/Gunmetal 3dfx/level0.sbw" "$d/"
> done
> ```
>
> Если слинковать только `PICS`/`sprites`/`LOOK16.BMP`, кадр оригинала
> отличается от кадра из установки на 42 пикселя из-за отсутствующих
> ресурсов — это шум каталога, а не правка. Поэтому оба кадра для сравнения
> сняты из полных каталогов.

### 5.2 `radius` объекта 0: 289.993 → 300

```sh
python3 tools/editor/sbwobj.py --set level0 --object 0 --field radius=300 \
    --out /tmp/opencode/sbwobj_full_radius/level0.sbw
```

```
/mnt/d/games/Gunmetal 3dfx/level0.sbw -> .../level0.sbw
obj[0] radius: 289.99310302734375 -> 300.0 (254439 bytes, unchanged size)
  [ok] oracle: source parses to EOF
  [ok] oracle: edited file parses to EOF
  ... (18 проверок, как выше)
  [ok] only radius bytes differ (span 26295..26299, 3 byte(s) changed)
  [ok] own walker sees radius=300.0
  [ok] oracle sees radius=300.0
  [ok] world vertices of the target object are unchanged
  note: radius differs from |bounds| by 10; the shipped data tolerates up to ~4129
        (bounds are rounded integers), cull uses radius
result: OK
```

Оракул независимо: изменены байты `[26295, 26296, 26297]`, `radius`
`289.99310302734375 -> 300.0`, `bounds`/`origin`/вершины не тронуты, мировые
вершины всех объектов идентичны. Движок: `--info` те же числа, строка
`bounds:` не изменилась (радиус в неё не входит), рендер 320×200 **совпал с
оригиналом побайтово (0 из 64 000 пикселей)**.

Почему рендер не изменился — это не «правка проглочена»: C++-рендер читает
`radius` в `engine/src/res/sbw.cpp:180`, но нигде не использует (в мире его
применяет только оригинальный движок для cull `0x437d8c`/`0x437d05` и сферы
сектора `0x4360f1`). Байтовая правка доказана оракулом, загрузкой и
неизменностью всего остального; видимость в кадре у `radius` в нашем
рендере отсутствует по построению.

---

## 6. Проверки

### 6.1 `--self-test` (без файлов игры), 82/82

Синтетический `.sbw` строится самим тестом (2 текстуры, 1 эффект, 2 сектора,
4 объекта, 4 полигона) и проверяется обоими читателями; затем:

* раскладка: `origin` на `header+0x04` (3 × f64), `basis` на `header+0x1C`
  (9 × f64), `n_verts`/`n_polys` и вершины от `geom_off`, `radius` на
  `tail+0x00`, `f75`/`f74` на `tail+0x04`/`+0x05`, `f7c`/`f78` на
  `tail+0x06`/`+0x0A`, `bounds` на `tail+0x0E`, `reserved` — 24 нуля;
  размеры записей;
* 11 правок (три `origin`, два `radius`, два `bounds`, `f74`/`f75`/`f78`/`f7c`)
  — каждая проходит все 18 проверок `verify_edit`;
* `basis=identity` — принят и проверен как no-op;
* отказы: `origin` из двух/четырёх чисел, `nan`/`inf`/не-число, нереально
  большой `origin`; отрицательный/`nan`/непомещающийся `radius`; отрицательные
  и `nan` `bounds`; `f74/f75/f78/f7c` вне 0..255 и нецелые; явный поворот и
  масштаб базиса; неизвестное поле; индекс объекта вне файла; запись внутрь каталога игры (27 отказов);
* обрезка на 10/60/200 байт и испорченный `n_verts` отвергаются и своим
  читателем, и оракулом;
* проверка не «пустая»: подмена вершины, чужого объекта и чужого поля в том же
  объекте ловится;
* полный цикл `--set` → `--check` через временные файлы, включая отказ без
  `--out`, отрицательный радиус, поворот базиса и неизвестное поле.

```
$ python3 tools/editor/sbwobj.py --self-test | tail -1
self-test: 82/82 passed
```

### 6.2 `--roundtrip` и `--audit`

```
$ python3 tools/editor/sbwobj.py --roundtrip
round-trip: 36/36 identical, 28192 objects, 357818 vertices, 235069 polygons, 40037031 bytes (/mnt/d/games/Gunmetal 3dfx)   # 2.5 s
```

Проверяется не только `Sbw.to_bytes() == data`, но и что свой читатель доходит
до EOF, counts совпадают с оракулом и **каждое** редактируемое поле
(`origin`/`basis`/`radius`/`bounds`/`f74`/`f75`/`f78`/`f7c`) у всех 28 192
объектов читается обоими читателями одинаково.

```
$ python3 tools/editor/sbwobj.py --audit
audit: 36 files, 28192 objects (/mnt/d/games/Gunmetal 3dfx)
  origin      x [-37579.0, 47456.0]  y [-29776.0, 131144.0]  z [-77608.0, 27920.0]
  radius      [2.2360680103302, 26801.943359375]
  bounds      x [0.0, 16000.0]  y [0.0, 23744.0]  z [0.0, 10488.0]
  basis       identity 28192/28192, non-identity 0
  f74 [0]  f75 [0, 1]  f78 [0]  f7c [0]
  id words    0x0, 0x8, 0x10, 0x20, 0x10000, 0x10008, 0x10010, 0x10020, 0x100000
  radius vs |bounds|: exact 5919/28192, <=2 22615/28192, max 4129 (level1.sbw obj 0)
result: OK -- every editable object field is inside its range
```

`--audit` — доказательство диапазонов из §2: все 28 192 объекта всех 36
файлов проходят `validate` (0 нарушений). Про `radius vs |bounds|`: в
`docs/FORMATS.md` §10 и `docs/WORLD_FIELDS.md` §2.3 приводятся числа
20 736 и 21 125 «точно» без указания допуска; наш замер в f64 с явными
порогами: `<1e-6` — 5 919, `<=0.5` — 21 537, `<=1` — 22 073, `<=2` — 22 615.
То есть `bounds` действительно округлены до целых, а у отдельных крупных
объектов (максимум — `level1.sbw` obj 0) поле `radius` расходится с `|bounds|`
на 4129. Инструмент поэтому **не требует** `radius == |bounds|`, а только
предупреждает, если после правки разница больше 2.

### 6.3 Регресс тулкита

```
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 65.7, "full": false, "categories": [],
 "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

Новых проверок в `regress.py` этот срез не добавляет (`regress.py` — общий
файл, его ведёт основной агент); профильные проверки живут в `--self-test`,
`--roundtrip`, `--audit`.

---

## 7. Ограничения

* **Вершины и полигоны отдельно не двигаются.** `--set origin` двигает объект
  целиком (вершины локальные, остаются те же). Локальные вершины не
  редактируются: это сломало бы кэш кадра полигонов
  (`centre`/`normal`/`u_axis`/`v_axis` — авторские; `bounds` и углы
  пересчитывает только `0x43529f`) и потребовало бы пересчёта
  `radius`/`bounds`/AABB сектора.
  Полигоны не добавляются и не удаляются, `n_verts`/`n_polys` не меняются:
  вставка сдвинула бы весь поток.
* **Произвольный базис не поддержан** (`basis=identity` — единственное
  значение). Поворот/масштаб потребовал бы согласованного пересчёта
  объект-локального кэша кадра, `radius`/`bounds`, AABB сектора и проверки на
  движке; поставочных примеров нет — такой правки нет и в инструменте.
* **`radius`/`bounds` — метаданные.** Движок строит AABB сектора из
  `origin ± bounds` (`0x434fed`) и отсекает по `radius` (`0x437d8c`), а
  геометрию — из вершин. Если править `bounds`/`radius` без вершин, можно
  получить объект, который отсекается не по фактической геометрии. Инструмент
  это не «исправляет»: он пишет ровно запрошенное и предупреждает о разнице
  `radius` и `|bounds|`.
* **Collision/portal и сцена не пересчитываются.** Правка меняет только
  `.sbw` (стадия 1 + хвост стадии 2). Записи `.sbt` (`doors`/`switches`/`lifts`
  ссылаются на `(sector, object)` и имеют собственные координаты), сфера
  сектора и любые порталы/объёмы, посчитанные из старых чисел, остаются
  прежними. Для чистого переноса объекта это согласовано (вершины
  трансформируются в мире тем же `origin + basis·local`), но правка
  `bounds`/`radius` без правки геометрии — это «несогласованные метаданные».
* **`f78`/`f7c` — u32, но инструмент принимает 0..255** (как и `f74`/`f75`):
  в поставке все три слова равны 0, назначения у них нет (читателей в коде
  мира не найдено), а `0x100000`-подобные биты доказаны только для
  id-слова. Расширять диапазон без доказательства не стали; при необходимости
  это одна строка в `validate`.
* **`f75`/`f74`/`f78`/`f7c` пишутся, но их влияние на движок не проверено.**
  `f74` читается как знаковая поправка яркости (`0x43c51f`/`0x43d8e3`),
  `f75` ненулевое у 7 поставочных объектов без найденного читателя,
  `f78`/`f7c` — без читателей. Инструмент разрешает их правку как байтов,
  но «что это меняет в игре» — не доказано.
* **`radius` в нашем рендере не используется** (§5.2): видимость правки
  подтверждается оракулом и загрузкой, но не кадром. Паритет рендера с
  оригиналом — предмет `docs/RENDER.md`.
* **`--check` без `--against`** сверяет только структуру (EOF, counts,
  round-trip); побайтовое сравнение с источником включается `--against LEVEL`
  (или автоматически, если рядом с `--game` есть файл с тем же именем).
* Секторы (`kind`/`sub`) и id-слово объекта не редактируются (§2): первое —
  не объект, второе — битовое поле без доказанных безопасных комбинаций.

---

## 8. Сводка команд

```sh
# что внутри уровня: объекты с origin/basis/radius/bounds/AABB
python3 tools/editor/sbwobj.py --list level0 --limit 8
python3 tools/editor/sbwobj.py --list level0 --limit 0 --json

# карта редактируемых полей с provenance (док + VA)
python3 tools/editor/sbwobj.py --fields

# чтение одного поля
python3 tools/editor/sbwobj.py --get level0 --object 0 --field origin
python3 tools/editor/sbwobj.py --get level0 --object 0 --field radius --json

# перенос объекта целиком (+64 по X) и проверка
python3 tools/editor/sbwobj.py --set level0 --object 0 \
    --field origin=1496,7568,48 --out /tmp/opencode/sbwobj/level0.sbw
python3 tools/editor/sbwobj.py --check /tmp/opencode/sbwobj/level0.sbw \
    --against level0 --object 0 --field origin=1496,7568,48

# радиус/коробка/вспомогательные слова
python3 tools/editor/sbwobj.py --set level0 --object 0 --field radius=300 \
    --out /tmp/opencode/sbwobj_radius/level0.sbw
python3 tools/editor/sbwobj.py --set level0 --object 0 --field bounds=184,184,128 \
    --out /tmp/opencode/sbwobj/level0.sbw
python3 tools/editor/sbwobj.py --set level0 --object 0 --field f75=1 \
    --out /tmp/opencode/sbwobj/level0.sbw

# корпус, диапазоны, самотест
python3 tools/editor/sbwobj.py --roundtrip
python3 tools/editor/sbwobj.py --audit
python3 tools/editor/sbwobj.py --self-test
```
