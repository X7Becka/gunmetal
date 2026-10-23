# Вершины `.sbw`: редактор (`M11.2`, срез 4)

* **Статус:** срез 4 готов: перечисление объектов/вершин/полигонов с
  локальными и мировыми координатами, правка **одной локальной вершины** и
  **перенос всех вершин объекта**, пересчёт `radius`/`bounds` по фактическим
  вершинам, побайтовая проверка правки двумя читателями, round-trip **36/36**,
  self-test **81/81**, правки `level0.sbw` подтверждены оракулом `gm.sbw`,
  загрузкой и рендером C++-движка.
* **Ветка:** `feat/mesh`, база `master` `cea78ab`, дата 2026-10-23.
* **Файлы:** `tools/editor/sbwmesh.py` (новый), `docs/SBW_MESH.md` (этот
  документ). Существующие файлы (`gm/*.py`, `tools/**/*.py`, `docs/*.md`,
  тесты, `engine/*`, `gm.py`, `regress.py`, `ROADMAP.md`) **не менялись**.
* **Соседние срезы:** объект целиком — `tools/editor/sbwobj.py`,
  `docs/SBW_OBJECTS.md`; поля полигонов — `tools/editor/sbwedit.py`,
  `docs/SBW_EDIT.md`. Здесь правятся **вершины**, а не объект и не полигон.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/mesh`
  (в git не попадают).
* **Оракул:** `gm/sbw.py` (`parse`, `Object.vertices`,
  `Object.world_vertices`, `Poly.indices`, `Sbw.to_bytes`) — эталон раскладки.

---

## 1. Где лежат вершины

`.sbw` — один последовательный поток (`gm/sbw.py:1-128`,
`docs/FORMATS.md` §10). Объект присутствует в файле дважды:

```
стадия 1 (трансформ):
    u32 flags;              // в gm/sbw.py — Object.id; в рантайме +0x70 (битовое поле)
    u8  header[96];         // 12 x f64: origin[3] + basis[3][3] (row-major)

стадия 2 (геометрия, тот же порядок сектор -> объект):
    u16 n_verts; u16 n_polys;
    f32 verts[n_verts][3];  // <-- ЛОКАЛЬНЫЕ вершины, 12 байт каждая
    f32 radius;             // +0x60
    u8  f75; u8 f74;        // +0x75 / +0x74
    u32 f7c; u32 f78;       // +0x7c / +0x78
    f32 bounds[3];          // +0x84, полуразмеры AABB вокруг origin
    u8  reserved[24];       // +0x90
    n_polys * { /* запись полигона, 124 + 2*n_idx байт */ }
```

Вершина — это **12 байт внутри записи фиксированного размера**
(`gm/sbw.py:442-448`: `n_verts`, затем `12*n_verts` байт, затем 50-байтный
хвост). Поэтому правка вершины — замена 12 байт на месте: `n_verts`/
`n_polys` не меняются, ни один байт не сдвигается, длина файла та же
(:func:`patch_vertex`, :func:`patch_translate`).

Мировой кадр: `world = origin + basis · local` (`Object.world_vertices`,
`gm/sbw.py:222-242`; трансформ `0x437af8` читает заголовок как `FLD double`,
вершину — как `FLD float`). `origin`/`basis` — **f64**, вершины — **f32**;
базис единичный во всех 28 192 поставочных объектах (`--audit`: 0
неединичных), то есть в поставке мир = `origin + local`.

Абсолютные смещения на примере `level0.sbw`, объект 0 (`--list --object 0`
печатает их для любого объекта):

| что | диск | размер |
|---|---|---:|
| `n_verts`/`n_polys` | `26195` (`geom+0x00`) | 4 |
| вершины | `26199` (`geom+0x04`), шаг 12 | `12*n_verts` = 96 |
| `radius` | `26295` (`tail+0x00`) | 4 |
| `f75` / `f74` | `26299` / `26300` | 1 + 1 |
| `f7c` / `f78` | `26301` / `26305` | 4 + 4 |
| `bounds` | `26309` (`tail+0x0E`) | 12 |
| `reserved` | `26321..26345` | 24 |
| первый полигон | `26345` | `124 + 2*n_idx` |

Вершина `i` объекта лежит по `verts_off + 12*i`; инструмент считает это
смещение сам (:meth:`ObjectLoc.vertex_span`) и проверяет его в self-test
(`layout: ...`).

---

## 2. Что правится и что доказывает `--check`

### 2.1 `--set-vertex` — одна локальная вершина

Меняются ровно 12 байт вершины. Мировая точка следует за ней:
`world_new = origin + basis · pos`. Остальные вершины объекта и все полигоны
не трогаются.

### 2.2 `--translate` — все вершины объекта

К каждой локальной вершине прибавляется дельта: `v_i' = f32(v_i + delta)`.
**Заголовок (`origin`/`basis`) не меняется** — это принципиальное отличие от
`sbwobj.py --field origin`: там двигался объект (мир сдвигался, локальные
координаты оставались), здесь двигается геометрия **внутри** системы объекта
(локальные координаты меняются, `origin` тот же). Мир тоже сдвигается на
`basis · delta`, но `radius`/`bounds` при этом обязаны вырасти: сфера/бокс
отсчитываются от `origin`, а геометрия от него отошла (§3).

### 2.3 `radius`/`bounds` пересчитываются после каждой правки

См. §3. Инструмент не оставляет «старые» метаданные: `radius` и `bounds`
всегда равны пересчёту по новым вершинам. Поэтому `--check` дополнительно
требует, чтобы **у всех** объектов файла `radius`/`bounds` совпадали с
правилом пересчёта (§3), — это ловит и «полуправку», и чужой писатель.

### 2.4 `verify_edit` — 23–24 обязательные проверки

(24 для `--set-vertex`, 23 для `--translate`: у переноса нет отдельной
проверки «оракул видит целевую вершину».)

1. оракул разбирает исходный и новый файл ровно до EOF;
2. counts (текстуры/эффекты/секторы/объекты/полигоны/**вершины**) и
   флаговые слова объектов не изменились;
3. размер файла не изменился;
4. заголовок целевого объекта (`u32` + 96 байт трансформа) идентичен;
5. **все байты вне целевого объекта** (таблицы, заголовки секторов, чужие
   заголовки/вершины/хвосты/полигоны) идентичны;
6. **все остальные объекты** (заголовок + стадия 2) идентичны побайтово;
7. внутри целевого объекта меняются только разрешённые спаны: 12 байт
   вершины (одной или всех) + `radius` + `bounds`; значит `n_verts`/`n_polys`,
   записи полигонов, слова `f74`/`f75`/`f78`/`f7c` и `reserved` — те же;
8. каждая вершина несёт ровно ожидаемое новое значение в `f32`: для
   `--set-vertex` — целевая, для `--translate` — все;
9. `radius`/`bounds` в файле равны `recompute_meta` новых вершин;
10. записи полигонов целевого объекта побайтово идентичны;
11. оба читателя (свой `walk` и `gm.sbw`) видят новые вершины и метаданные;
12. `Sbw.to_bytes()` оракула побайтово равен исходному и новому файлу;
13. мировые вершины сдвинулись ровно на `basis · (new − old)` (для
    `--translate` — для всех, для `--set-vertex` — для одной; допуск `1e-3`,
    на поставочных данных фактически 0), прочие вершины объекта и **все**
    другие объекты в мире не сдвинулись.

Проверка «мировые вершины» — содержательное доказательство: правка локальна,
мир меняется ровно на дельту, остальное стоит.

`--check FILE --against LEVEL [--object N --index I --pos .... | --delta ...]`
повторяет те же проверки на готовом файле; без ожидания правки требует
полного побайтового равенства источнику. `--set-vertex`/`--translate` всегда
пишут в `--out` и отказываются писать внутрь каталога игры (`--game`), даже
если такой `--out` передан явно.

---

## 3. Пересчёт `radius`/`bounds` по фактическим вершинам

`docs/SBW_OBJECTS.md` §2 и `docs/WORLD_FIELDS.md` §2.3 описывают эти поля как
метаданные, которые читает cull: `radius` — `0x437d8c`/`0x437d05` и сфера
сектора `0x4360f1`, `bounds` — AABB сектора `0x434fed` (`origin ± bounds`).
После правки вершин они обязаны соответствовать новой геометрии, иначе объект
отсекается не по себе.

Инструмент считает так (движковый пересчёт `0x43529f`: вершина **минус
origin**, затем `FABS`; `docs/WORLD_FIELDS.md` §2.3):

```
b_i      = basis · local_i                       // в поставке basis = identity
bounds[k] = f32( max_i |b_i[k]| )                 // max(|x|), max(|y|), max(|z|)
radius    = f32( max_i |b_i| )                    // до самой дальней вершины
```

То есть `bounds` — полуразмер AABB **вокруг origin объекта** (а не
`(max−min)/2`), `radius` — расстояние до дальней вершины от того же origin.

**Доказательство правила — данные, не догадка.** На всех 36 файлах
(`--audit`, 28 192 объекта, 357 818 вершин):

```
$ python3 tools/editor/sbwmesh.py --audit
audit: 36 files, 28192 objects, 357818 vertices, 235069 polygons (/mnt/d/games/Gunmetal 3dfx)
  local coords max |x|,|y|,|z| = 16000, 23744, 10488
  radius max = 26801.9; basis non-identity = 0
  vertices referenced = 357818/357818 (unused 0), max polys/vertex = 16, bad indices = 0, zero-vertex objects = 0
  radius/bounds == recompute rule: 28192/28192
result: OK -- the recompute rule reproduces every shipped object bit-exactly
```

* `bounds` = `max(|x|),max(|y|),max(|z|)` — совпало **побитово у 28 192/28 192**
  объектов (это же правило зафиксировано в `docs/WORLD_FIELDS.md` §2.3).
* `radius` = `max|v|` — совпало **побитово у 28 192/28 192**.
* `radius` = `|bounds|` (как записано в `docs/FORMATS.md` §10 и
  `docs/WORLD_FIELDS.md` §2.3) — точно у **20 736/28 192** (то же число, что в
  документе). Расхождение не случайно: `|bounds|` — расстояние до **угла**
  AABB, а `radius` — до **фактической** дальней вершины; они совпадают, когда
  вершина реализует угол (симметричные коробки), и расходятся у «неполных»
  объектов (`level0` obj 11: `bounds=(123,8,60)`, но вершины с `x=123` имеют
  `z=52`, поэтому `radius=136.191…`, а `|bounds|=137.087…`). Инструмент пишет
  тесное правило (`max|v|`), которое воспроизводит поставку целиком.

**Меняет ли пересчёт поставку?** Для нетронутого файла — нет: правило
побитово равно записанным значениям у всех 28 192 объектов, поэтому `--check`
на любом поставочном файле проходит, а правка «в ту же точку» пишет
байт-идентичную копию (`no-op`, код 0). Для переноса (`--translate`) числа
меняются **обязательно**, потому что геометрия отошла от origin; фактические
числа для `level0` obj 0 — в §5.2 (`radius 289.993103 → 297.4021`,
`bounds (184,184,128) → (184,184,144)`).

---

## 4. CLI

```sh
python3 tools/editor/sbwmesh.py --list LEVEL [--object N] [--limit N] [--json]
python3 tools/editor/sbwmesh.py --set-vertex LEVEL --object N --index I \
    --pos x,y,z --out FILE
python3 tools/editor/sbwmesh.py --translate LEVEL --object N --delta dx,dy,dz \
    --out FILE
python3 tools/editor/sbwmesh.py --check FILE [--against LEVEL] \
    [--object N (--index I --pos x,y,z | --delta dx,dy,dz)]
python3 tools/editor/sbwmesh.py --roundtrip [--game DIR] [--verbose]
python3 tools/editor/sbwmesh.py --self-test
python3 tools/editor/sbwmesh.py --audit
```

`--object N` — глобальный 0-based индекс в порядке `--list` (сектор →
объект), тот же, что у `gm/sbw.py`. `--index I` — 0-based индекс вершины
внутри объекта. `--limit` ограничивает число показанных объектов и число
вершин/полигонов на объект (`0` — всё). `--game` по умолчанию
`/mnt/d/games/Gunmetal 3dfx`.

Коды возврата: `0` — успех; `1` — проверка не прошла (`--check`,
`--roundtrip`, `--self-test`, `--audit`); `2` — неверный ввод или отказ
(сообщение в stderr); `3` — внутренняя ошибка. Трассировка наружу не выходит.

`--list level0 --object 0 --limit 4`:

```
level: /mnt/d/games/Gunmetal 3dfx/level0.sbw
header: 254439 bytes, oracle parse ends at 254439 (EOF ok)
tables: textures=381 effects=37 (end 5320, 6585)
world: sectors=58 objects=192 vertices=2173 polygons=1449
objects:
  obj[    0] sector=  0 index=  0 flag=0x00000000 verts=   8 polys=   6 radius=289.993
    origin=(1432, 7568, 48) bounds=(184, 184, 128) basis=identity
    metadata: radius recomputed 289.993, bounds recomputed (184, 184, 128) (match)
    aabb=((1248, 7384, -80) .. (1616, 7752, 176))
    vertices (showing 4 of 8, unused=0, max polys/vertex=3, bad indices=0):
      v[  0] local=(184, 184, 128) world=(1616, 7752, 176) polys=3
      v[  1] local=(184, 184, -128) world=(1616, 7752, -80) polys=3
      v[  2] local=(-184, 184, 128) world=(1248, 7752, 176) polys=3
      v[  3] local=(-184, 184, -128) world=(1248, 7752, -80) polys=3
      ... 4 more (--limit 0 shows all)
    polygons (showing 4 of 6):
      p[  0] indices=(0, 2, 4, 6) texture=1 material=0 lightmap=0 disk=26345+132
      p[  1] indices=(7, 5, 3, 1) texture=1 material=0 lightmap=0 disk=26477+132
      p[  2] indices=(3, 2, 0, 1) texture=1 material=0 lightmap=0 disk=26609+132
      p[  3] indices=(5, 4, 2, 3) texture=1 material=0 lightmap=0 disk=26741+132
```

`polys=` у вершины — сколько полигонов объекта её используют (`--audit`:
неиспользованных вершин в поставке 0, максимум 16 полигонов на вершину, все
индексы в диапазоне `0..n_verts-1`). В `--json` те же данные плюс
`radius_recomputed`/`bounds_recomputed`, `meta_matches_recompute`, смещения
`verts_off`/`tail_off` и (при `--object N`) полный список полигонов с
индексами.

---

## 5. Правка на реальных данных (`level0.sbw`)

Каталоги для движка — полная установка, слинкованная по файлам, с реальным
`level0.sbw` (иначе кадр отличается из-за отсутствующих текстур — тот же
приём, что в `docs/SBW_OBJECTS.md` §5):

```sh
mkdir -p /tmp/opencode/mesh/{orig,vtx,tr}
for d in orig vtx tr; do
  for f in "/mnt/d/games/Gunmetal 3dfx"/*; do
    b="$(basename "$f")"
    case "$b" in level0.sbw|level0.sbt|level0.sbc) ;; *) ln -sfn "$f" "$d/$b";; esac
  done
  cp "/mnt/d/games/Gunmetal 3dfx/level0."{sbt,sbc} "$d/"
done
cp "/mnt/d/games/Gunmetal 3dfx/level0.sbw" /tmp/opencode/mesh/orig/
```

### 5.1 Одна вершина: obj 0, vertex 0, `+8` по X

```sh
python3 tools/editor/sbwmesh.py --set-vertex level0 --object 0 --index 0 \
    --pos 192,184,128 --out /tmp/opencode/mesh/vtx/level0.sbw
```

```
/mnt/d/games/Gunmetal 3dfx/level0.sbw -> /tmp/opencode/mesh/vtx/level0.sbw
obj[0] vertex[0]: (184, 184, 128) -> (192, 184, 128) (local; world (1616, 7752, 176) -> (1624, 7752, 176))
metadata: radius 289.993103 -> 295.133881, bounds (184, 184, 128) -> (192, 184, 128) (recomputed from the vertices)
254439 bytes, unchanged size
  [ok] oracle: source parses to EOF
  [ok] oracle: edited file parses to EOF
  [ok] oracle: texture/effect/sector/object/polygon/vertex counts unchanged
  [ok] oracle: object flag words unchanged
  [ok] own walker: both files to EOF
  [ok] own walker: counts (objects/vertices/polygons) unchanged
  [ok] file size unchanged
  [ok] target object header (u32 flag word + 96-byte transform) unchanged
  [ok] bytes outside the target object are identical
  [ok] 191 other objects (header + vertices + tail + polygons) are identical
  [ok] only vertex/radius/bounds bytes differ (5 byte(s) changed, all inside 3 legal spans)
  [ok] the file actually changed
  [ok] own walker sees vertex[0] = (192, 184, 128)
  [ok] 7 other vertices of the target object are unchanged
  [ok] radius equals max|vertex| = 295.1338806152344
  [ok] bounds equal max|component| = (192, 184, 128)
  [ok] 6 polygon records of the target object are byte-identical
  [ok] oracle sees the new vertex values
  [ok] oracle sees vertex[0] = (192, 184, 128)
  [ok] oracle sees the recomputed radius/bounds
  [ok] oracle round-trip of the source is exact
  [ok] oracle round-trip of the edit is exact
  [ok] no other object moved in the world frame (world = origin + basis * local)
  [ok] world vertices moved exactly by (8, 0, 0) (1 vertex, max deviation 0)
  note: vertex[0] local (184, 184, 128) -> (192, 184, 128); radius 289.993 -> 295.134; bounds (184, 184, 128) -> (192, 184, 128)
result: OK
```

`--check` на готовом файле:

```sh
python3 tools/editor/sbwmesh.py --check /tmp/opencode/mesh/vtx/level0.sbw \
    --against level0 --object 0 --index 0 --pos 192,184,128
```

```
check: /tmp/opencode/mesh/vtx/level0.sbw (254439 bytes)
against: /mnt/d/games/Gunmetal 3dfx/level0.sbw
target: obj[0] vertex[0]=(192, 184, 128)
  [ok] oracle: parses to EOF
  [ok] own walker: parses to EOF
  [ok] own walker and oracle agree on counts
  [ok] oracle: read -> write is byte-exact
  [ok] radius/bounds equal the recompute rule in every object (192 objects, 0 mismatch)
  ... (далее те же 24 проверки правки)
result: OK
```

Сверка **независимым** скриптом на `gm.sbw` (без кода инструмента):

```
same size: True parse EOF: True True
counts equal: True
obj0 radius 289.99310302734375 -> 295.1338806152344
obj0 bounds (184.0, 184.0, 128.0) -> (192.0, 184.0, 128.0)
differing bytes: [26201, 26295, 26296, 26297, 26311]
moved world vertices: [(0, (8.0, 0.0, 0.0))]
other objects' world vertices moved: []
every other object identical (header+tail+polys): True
every polygon record identical: True
target header (id+transform) identical: True | origin: (1432.0, 7568.0, 48.0) -> (1432.0, 7568.0, 48.0)
target local vertices changed at: [0]
```

То есть изменились **5 байт**: третий байт `f32` вершины 0 (`184.0 → 192.0`,
`26201`), три байта `radius` (`26295..26297`) и один байт `bounds.x`
(`26311`). Ни одна другая вершина, ни один полигон, ни один другой объект не
изменены; мир сдвинут ровно на `+8` по X у одной вершины.

### 5.2 Перенос всех вершин: obj 0, `(0,0,16)`

```sh
python3 tools/editor/sbwmesh.py --translate level0 --object 0 \
    --delta 0,0,16 --out /tmp/opencode/mesh/tr/level0.sbw
```

```
... -> /tmp/opencode/mesh/tr/level0.sbw
obj[0] translate by (0, 0, 16) (8 vertices)
metadata: radius 289.993103 -> 297.4021, bounds (184, 184, 128) -> (184, 184, 144) (recomputed from the vertices)
254439 bytes, unchanged size
  ... (все проверки [ok], включая:)
  [ok] target object header (u32 flag word + 96-byte transform) unchanged
  [ok] only vertex/radius/bounds bytes differ (16 byte(s) changed, all inside 10 legal spans)
  [ok] all 8 vertices moved by (0, 0, 16)
  [ok] radius equals max|vertex| = 297.402099609375
  [ok] bounds equal max|component| = (184, 184, 144)
  [ok] no other object moved in the world frame (world = origin + basis * local)
  [ok] world vertices moved exactly by (0, 0, 16) (8 vertices, max deviation 0)
  note: 8 vertices moved by (0, 0, 16); radius 289.993 -> 297.402; bounds (184, 184, 128) -> (184, 184, 144)
result: OK
```

`--check ... --delta 0,0,16` — `result: OK`. Независимый оракул:

```
same size: True parse EOF: True True
counts equal: True
obj0 radius 289.99310302734375 -> 297.402099609375
obj0 bounds (184.0, 184.0, 128.0) -> (184.0, 184.0, 144.0)
differing bytes: [26209, 26221, 26222, 26233, 26245, 26246, 26257, 26269, 26270, 26281, 26293, 26294, 26295, 26296, 26297, 26319]
moved world vertices: [(0, (0.0, 0.0, 16.0)), (1, (0.0, 0.0, 16.0)), ... , (7, (0.0, 0.0, 16.0))]
other objects' world vertices moved: []
every other object identical (header+tail+polys): True
every polygon record identical: True
target header (id+transform) identical: True | origin: (1432.0, 7568.0, 48.0) -> (1432.0, 7568.0, 48.0)
target local vertices changed at: [0, 1, 2, 3, 4, 5, 6, 7]
```

16 байт: 12 байт в `f32` z у 8 вершин (`128 -> 144` меняет один байт,
`-128 -> -112` — два, отсюда 12), три байта `radius` (`26295..26297`) и один
байт `bounds.z` (`26319`). `origin` не изменился.

### 5.3 Движок

Бинарь `build/sandbox/engine/gunmetal` (Debug-сборка `master` `cea78ab`).
`--info` на трёх каталогах (`orig`, `vtx`, `tr`) — **дословно одинаковый**:

```
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
lightmaps: magic=SBC1 records=405 texels=51792
```

Движок сам печатает AABB уровня по загруженным вершинам — правка вершины
видна и ему:

```
orig: bounds: (-3080, -408, -1920) .. (1616, 8836, 288)
vtx:  bounds: (-3080, -408, -1920) .. (1624, 8836, 288)
tr:   bounds: (-3080, -408, -1920) .. (1616, 8836, 288)
```

(`tr` границу уровня не меняет: перенос по Z не двигает `x`/`y`, а крайний
`z = 288` задаёт другой объект, не `obj 0`.)

Рендер 320×200 (`--render`, 192 меша, 1449 граней, 55 текстур, 0
отсутствующих) отличается; попиксельная сверка через `gm/png.py`
(`tools/re/frame_metric.py` сравнивает наш кадр с кадром **оригинальной
игры**, а здесь сравниваются два наших кадра, поэтому считаем прямым
побайтовым сравнением RGBA):

| кадр | отличающихся пикселей | доля | bbox отличий |
|---|---:|---:|---|
| `vtx` (vertex 0 → `+8 X`) | **517** из 64 000 | 0.81 % | `x 84..254, y 71..118` |
| `tr` (`--translate (0,0,16)`) | **79** из 64 000 | 0.12 % | `x 190..201, y 108..118` |

Оба кадра имеют тот же размер и те же `meshes/faces`; отличия лежат в
области, куда переехала геометрия `obj 0` (дальняя стена арены; у `tr` она
почти вся за другими объектами, отсюда маленькая площадь).

---

## 6. Проверки

### 6.1 `--self-test` (без файлов игры), 81/81

Синтетический `.sbw` строится самим тестом (2 текстуры, 1 эффект, 2 сектора,
4 объекта, 11 вершин, 4 полигона; один объект без вершин и полигонов, одна
вершина не используется полигонами), проверяется обоими читателями; затем:

* раскладка: `n_verts`/`n_polys` на `geom_off`, вершины от `geom_off+4`
  (12 байт, проверен спан вершины 3), `radius` на `tail+0x00`, `f75`/`f74` на
  `tail+0x04`/`+0x05`, `f7c`/`f78` на `tail+0x06`/`+0x0A`, `bounds` на
  `tail+0x0E`, `reserved` — 24 нуля; размеры записей;
* `world = origin + basis · local`; правило пересчёта точно на синтетике; учёт
  использования вершин полигонами (2/2/2/2 и 1/1/1/0); пересчёт через
  неединичный базис (`0x43529f`-порядок: базис, затем `FABS`);
* правки: вершина-угол (`+4` по Z, метаданные растут), внутренняя вершина
  (метаданные не меняются), `--translate` (все 4 вершины, заголовок цел,
  `bounds.z 4 → 20`), no-op вершины и нулевой перенос — байт-идентичные копии;
* отказы: индекс `== n_verts`, отрицательный и отсутствующий индекс, `pos`
  из двух/четырёх чисел, `nan`/`inf`/не-число, нереально большой `pos`/`delta`,
  объект вне файла, перенос объекта без вершин, неизвестный вид правки,
  `--set-vertex` без `--pos`, запись внутрь каталога игры (20 отказов);
* обрезка на 10/60/200 байт и испорченный `n_verts` отвергаются и своим
  читателем, и оракулом;
* проверка не «пустая»: подмена другой вершины, неверное значение целевой,
  правка списка индексов полигона, устаревший `radius`, изменение `f75`,
  изменение чужого объекта и заголовка (`origin`) — всё ловится;
* полный цикл `--set-vertex` → `--check` и `--translate` → `--check` через
  временные файлы, включая отказ по неверному `pos`/`delta`, «vertex на
  translate», полное равенство источника и отказы без `--out`;
* `--list` на синтетике: строки вершин несут local/world/usage, метаданные
  сходятся с правилом, `--object` фильтрует.

```
$ python3 tools/editor/sbwmesh.py --self-test | tail -1
self-test: 81/81 passed
```

### 6.2 `--roundtrip` (36/36)

```
$ python3 tools/editor/sbwmesh.py --roundtrip
round-trip: 36/36 identical, 28192 objects, 357818 vertices, 235069 polygons, 40037031 bytes (/mnt/d/games/Gunmetal 3dfx)
metadata: radius/bounds equal the recompute rule in 28192/28192 objects
# ~2.9 s
```

Проверяется не только `Sbw.to_bytes() == data`, но и что свой читатель доходит
до EOF, counts совпадают с оракулом, для **каждого** объекта совпадают
`origin`/`basis`/вершины/`radius`/`bounds`/индексы полигонов у обоих
читателей, и что `radius`/`bounds` равны правилу §3 побитово.

### 6.3 `--audit`

Вывод — в §3. Дополнительно измерено: локальные координаты не выходят за
`(16000, 23744, 10488)`, `radius` до `26801.9`, неиспользованных вершин 0,
индексов вне диапазона 0, объектов без вершин 0, базис неединичный 0.

### 6.4 Регресс тулкита

```
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 48.7, "full": false, "categories": [],
 "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

Новых проверок в `regress.py` этот срез не добавляет (`regress.py` — общий
файл, его ведёт основной агент); профильные проверки живут в `--self-test`,
`--roundtrip`, `--audit`.

---

## 7. Ограничения

* **Вершины и полигоны не создаются и не удаляются.** `n_verts`/`n_polys` не
  меняются: запись объекта фиксированного размера, вставка/удаление сдвинули
  бы весь последовательный поток (и потребовали бы переиндексации полигонов).
  Инструмент отказывает по индексу вне `0..n_verts-1` текстом и кодом 2.
* **Кэш кадра полигонов не пересчитывается.** `centre`/`normal`/`u_axis`/
  `v_axis`/`corner_a`/`corner_b` — авторские поля в объект-локальном кадре
  (`docs/FORMATS.md` §10.1); после сдвига вершины полигон, её использующий,
  сохраняет старые центр/нормаль/UV-оси/углы. Движок пересчитывает при
  загрузке только `bounds`/углы (`0x43529f`), поэтому нормаль и UV остаются
  «от прежней» геометрии. Это видно в рендере и является осознанным
  ограничением среза: пересчёт нормалей/UV — отдельная задача (нужны
  доказательства по `0x43e02d`/`0x43529f` и проверка на всех полигонах).
  Winding при этом не меняется (индексы те же), см. `docs/COLLIDE.md` §1.2:
  сохранённая нормаль и нормаль обхода расходятся и в поставочных данных.
* **Collision-кэш движка не трогается.** Инструмент пишет только `.sbw`.
  Наш C++ `CollisionWorld::build` строит треугольники из вершин при загрузке
  (правку увидит), но любые рантайм-кэши оригинального движка (AABB секторов,
  сферы, порталы) в файл не входят и инструментом не обновляются. AABB
  сектора в оригинале строится только по объекту 0 (`0x434fed`), поэтому
  правка вершин **не** объекта 0 его не двигает.
* **`radius`/`bounds` — производные.** Инструмент всегда пересчитывает их по
  вершинам; вручную задать «несовпадающие» метаданные им нельзя (для этого —
  `sbwobj.py --field radius/bounds`, который пишет ровно запрошенное и
  предупреждает о расхождении). После `--translate` метаданные **обязаны**
  измениться (§5.2) — это не дефект, а корректный cull вокруг `origin`.
* **Мир-правка объекта — через `origin`.** Если нужно переместить объект, не
  меняя локальных координат (и не раздувая `radius`/`bounds`), это
  `sbwobj.py --field origin=...`; `sbwmesh.py --translate` двигает геометрию
  внутри системы координат объекта.
* **Базис не правится** и не требуется единичным для чтения/правки: пересчёт
  метаданных применяет базис, как `0x43529f`. В поставке он единичный у
  28 192/28 192 объектов — неединичных примеров и проверки на движке нет,
  поэтому вращать/масштабировать объект инструмент не умеет.
* **Сцена/скрипты не пересчитываются.** Записи `.sbt`
  (`doors`/`switches`/`lifts`, координаты и ссылки `(sector, object)`) и
  лайтмапы `.sbc` остаются прежними; сдвиг вершины не двигает ни дверь, ни
  триггер, ни запечённый свет.
* **`--check` без `--against`** сверяет только структуру (EOF, counts,
  round-trip, правило метаданных); побайтовое сравнение с источником
  включается `--against LEVEL` (или автоматически, если рядом с `--game` есть
  файл с тем же именем).
* **Точность.** Вершина в файле — `f32`; `--pos 184.123456` запишется как
  `184.123459…`, и мир сдвинется на фактическую `f32`-дельту. Это фиксируется
  в выводе (`world ... by (0.123459, 0, 0)`, deviation 0) и проверяется
  допуском `1e-3` в мировом кадре.

---

## 8. Сводка команд

```sh
# что внутри уровня: объекты, вершины (local/world/usage), полигоны
python3 tools/editor/sbwmesh.py --list level0 --limit 8
python3 tools/editor/sbwmesh.py --list level0 --object 0 --limit 0
python3 tools/editor/sbwmesh.py --list level0 --object 0 --limit 0 --json

# одна локальная вершина (+8 по X) и проверка
python3 tools/editor/sbwmesh.py --set-vertex level0 --object 0 --index 0 \
    --pos 192,184,128 --out /tmp/opencode/mesh/vtx/level0.sbw
python3 tools/editor/sbwmesh.py --check /tmp/opencode/mesh/vtx/level0.sbw \
    --against level0 --object 0 --index 0 --pos 192,184,128

# все вершины объекта (0,0,16), origin не меняется
python3 tools/editor/sbwmesh.py --translate level0 --object 0 --delta 0,0,16 \
    --out /tmp/opencode/mesh/tr/level0.sbw
python3 tools/editor/sbwmesh.py --check /tmp/opencode/mesh/tr/level0.sbw \
    --against level0 --object 0 --delta 0,0,16

# весь корпус, правило метаданных, самотест
python3 tools/editor/sbwmesh.py --roundtrip
python3 tools/editor/sbwmesh.py --audit
python3 tools/editor/sbwmesh.py --self-test

# движок на правке (полная установка, слинкованная по файлам)
build/sandbox/engine/gunmetal -g /tmp/opencode/mesh/vtx -l level0 --info
build/sandbox/engine/gunmetal -g /tmp/opencode/mesh/vtx -l level0 \
    --render /tmp/opencode/mesh/vtx.png --width 320 --height 200
```
