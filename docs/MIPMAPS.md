# Mip-уровни, фильтрация и прозрачный индекс

Статус: часть `M4.2.3` закрыта в Python-вьювере (`gm/render.py`), без изменения
поведения по умолчанию. C++-рендерер и `gm.py` не менялись.

Коротко:

* `.sbm` уже содержит mip-цепочку; загрузчик `gundx.exe` читает её целиком и
  отдаёт в Glide.
* Игра включает **mipmapping**, **билинейную фильтрацию** и **LOD bias -0.75**;
  выбор конкретного уровня делает Glide, а не CPU-код.
* Прозрачный индекс — **0**; загрузчик сам проверяет его наличие и для обычных
  `PICS`-текстур отключает прозрачность, если нулевых текселей слишком мало.
* Новые режимы в `gm/render.py` включаются только явными аргументами
  (`mip`, `filter`, `alpha_test`). Старый вызов идёт по прежней ветке кода и
  даёт побитово те же PNG (проверяется `tools/parity_mip.py`).

## 1. Mip-цепочка в `.sbm`

Заголовок (подтверждён на всех 1827 блобах, `gm/sbm.py`):

```
u32 zero;
u8  log2_width;
u8  log2_height;
u8  levels_minus_one;   // уровней = levels_minus_one + 1
u8  zero2;
u32 flags;
u8  pixels[];           // уровень 0, затем (w>>1)x(h>>1), ... до 1x1
```

Размер уровня `k` — `max(1, w >> k) * max(1, h >> k)`. Цепочка хранится в самом
файле; `sbm.parse()` ничего не досчитывает.

Факты по архивам (посчитаны по `PICS/SBM.IDX` и `sprites/SBM.IDX`):

| архив | записей | уровней | с индексом 0 |
|---|---:|---|---:|
| `PICS` | 1648 | 1..9 (1645 записей > 1 уровня) | 130 |
| `sprites` | 179 | всегда 1 | 178 |

То есть мипы есть почти у всех текстур уровней, а у спрайтов их нет — спрайты
всегда рисуются с уровня 0.

## 2. Что делает `gundx.exe` (доказательства)

### 2.1. Загрузка цепочки

`0x422462` — построение mip-объекта текстуры:

* аргументы — `log2_width`, `log2_height`;
* выделяется `(levels_minus_one + 1) * 16` байт;
* для каждого уровня пишутся `log2w`/`log2h` (`+0xd`/`+0xe`) и маска
  `(1 << (log2w + log2h)) - 1` (`+0x4`, то есть `w*h - 1`);
* затем одним блоком выделяются сами пиксели, уровни получают указатели.

Загрузчик `0x42356c` (цикл по слотам текстур):

* вызывает `0x422462` (`0x423814`) и передаёт число уровней из `.sbm`
  (`levels_minus_one`, байт `+6` слота);
* для каждого уровня вызывает `0x4805a0` (`0x423b42`, `0x423ec8`) — загрузку
  уровня в текстурную память;
* `0x47e420` (`0x423b1c`) готовит/перекодирует уровень (палитра + пиксели) перед
  загрузкой.

Это доказывает, что движок использует не только уровень 0: вся цепочка
загружается в texture memory.

### 2.2. Состояние Glide

Glide-бэкенд подгружается динамически (`glide2x.dll`, строки `_grTex...@`,
GetProcAddress-цикл `0x416a80`..`0x416e20`). Настройки сэмплера:

| VA | вызов | аргументы |
|---|---|---|
| `0x418513` | `grTexLodBiasValue(0, -0.75)` | `0xbf400000` = -0.75f |
| `0x418690` | `grTexFilterMode(0, 1, 1)` | билинейная min/mag |
| `0x4187be` | `grTexDownloadTable(0, 2, table)` | палитра |
| `0x4187e4`..`0x4187ea` | `grTexMipMapMode(0, 2, 0)` | mode 2 = `GR_MIPMAP_NEAREST_DITHER`, `pointSampled = FALSE` |
| `0x41883a` | `grTexClampMode(0, 0, 0)` | wrap S/T |

Имена констант (`GR_MIPMAP_NEAREST_DITHER = 2`, `GR_TEXTUREFILTER_BILINEAR = 1`,
`GR_TEXTURE_WRAP = 0`) — из заголовков Glide 2.x; в exe видны только числовые
литералы `2`, `1`, `0`. Поэтому «что это за режим» — доказано по числам и
вызовам, а точная формула LOD внутри драйвера — **гипотеза/внешняя спецификация**.
`pointSampled = FALSE` означает, что драйверу разрешено смешивать два соседних
mip-уровня (trilinear); наш вьювер это пока не повторяет.

Итог: в оригинале уровень выбирается **аппаратно по экранным производным** с
bias -0.75, внутри уровня — билинейно, между уровнями — ближайший/трилинейный
режим Glide. CPU-кода «посчитать номер мипа» в `gundx.exe` нет.

### 2.3. Прозрачный индекс

Загрузчик `0x423947`..`0x423a40`:

```
for each texel of level 0:
    if texel == 0: type = 1; zero_count++
if type == 1 and kind == PICS and
   zero_count < (1 << min(log2w, log2h)):
        "Warning: Illegal transparent texture %s"
        type = 0            # прозрачность отключена
```

* `0x4239cc` — сравнение тексела с нулём;
* `0x4239d0` — `[slot+4] = 1` (тип «transparent»);
* `0x4239e4`..`0x423a25` — эвристика и предупреждение
  `0x4a8d88 "Warning: Illegal transparent texture %s\n"`;
* `0x423a40`..`0x423a93` — имена на `S`/`s` + `K`/`k` + `Y`/`y` дают тип 2
  («sky»), а `W`/`w` + `A`/`a` + `T`/`t` — бит `0x10` (water).

Тип текстуры хранится в слоте: `+4` = 0/1/2 (opaque/transparent/sky), `+5` =
вид (0 `pics`, 1 `sprites`, 4 `flag` — видно по строкам `pics\%s.sbm`,
`sprites\%s.sbm`, `flag\%s.sbm`, `0x4a8db4`/`0x4a8dc0`/`0x4a8dcc`). Проверка
типа 2 в рендере есть, например, в `0x4244b3`, `0x4398be`, `0x43e7a7`.

Таким образом, **индекс 0 доказан кодом** (и данными: 178/179 спрайтов и 130
PICS-текстур содержат нули). Для обычных `PICS` движок применяет эвристику
«слишком мало нулей — не прозрачная», поэтому в вьювере alpha-test включается
только явно.

Полигонный флаг `+0x7c & 0x2` в поставке встречается (например, 318 из 1449
полигонов `level0`, 372 из 5786 `dm1`). В `docs/FORMATS.md` он назван
«прозрачная (sprites) текстура», но статический поиск не нашёл прямого чтения
этого бита в пути растеризации; поэтому вьювер **не** использует его как
триггер alpha-test — это была бы гипотеза, а не доказательство.

## 3. Реализация в `gm/render.py`

### API

```python
render.render(..., mip=0, filter="nearest", alpha_test=False,
              transparent_index=0, lod_bias=-0.75)
```

* `mip=0` — уровень 0 (историческое поведение);
* `mip=N` — фиксированный уровень, зажимается в `[0, levels-1]`;
* `mip="auto"` — экранный LOD (см. ниже);
* `filter="nearest"` / `"bilinear"` (синонимы `linear`, `smooth`);
* `alpha_test=True` — отбрасывать тексели с индексом `transparent_index`
  (по умолчанию 0); `transparent_index=None` отключает тест;
* `lod_bias` применяется только при `mip="auto"`; -0.75 — значение из Glide.

`TexturePool.get()` по-прежнему возвращает `(w, h, rgb)` уровня 0.
`TexturePool.get_texture()` возвращает `Texture` с `indices(level)`,
`rgb(level)`, `size(level)`, `levels`, `kind` и `transparent_index`.

### Выбор уровня

Для каждого треугольника берутся экранные координаты вершин `p_i` и
текстурные координаты в текселях `t_i = (u_i, v_i) * (w, h)` уровня 0.
Максимальная производная по рёбрам:

```
g = max over edges ( |Δ t| / |Δ p| )
LOD = log2(g) + lod_bias
level = clamp(round(LOD), 0, levels - 1)
```

Это консервативная оценка «самого грубого ребра», постоянная на треугольник.
Она не равна попиксельным производным Glide, но даёт тот же порядок величины и
не «мылит» сильнее, чем нужно. Для 1x1 и текстур с одним уровнем всегда
возвращается уровень 0.

### Билинейная фильтрация

Внутри выбранного уровня — стандартная схема `u*w - 0.5`, четыре соседних
текселя, wrap по маске степени двойки. Цвета смешиваются как RGB, а альфа —
как вес непрозрачности (0 или 1 на тексель); при `alpha_test` пиксель
отбрасывается, если вес `<= 0.5`. Это осознанное приближение: Glide считает
альфу палитры отдельно.

### Совместимость

Если `mip == 0`, `filter == "nearest"` и `alpha_test == False`, вызывается
старая ветка `_triangle` без нового сэмплера. Поэтому все старые вызовы
(включая `gm.py view` и `regress.py`) дают те же байты. Это проверено
golden-хешами ниже.

## 4. Паритет

`python3 tools/parity_mip.py` пересчитывает FNV-1a 64 по RGB (не по PNG) и
сверяет с константами C++-теста `engine/tests/test_main.cpp`. Уровни рендерятся
в world-кадре (`transform=True`), как в C++.

| кадр | параметры | FNV-1a 64 |
|---|---|---|
| `20mmclp.sob` | 160x160 | `c376475e890d6879` |
| `MOCLASS.SOB` | 160x160 | `dff93879e03ce1ab` |
| `code492.sob` | 160x160 | `d3fa091674e86a7a` |
| `desertpc.sob` | 160x160 | `1de0c4538c49b7c7` |
| `moonfb.sob` | 160x160 | `4fb421bc5a2ff221` |
| `level0` | 320x200, orbit | `20e80d077fd4f6bc` |
| `level0` | 320x200, pos (0,0,600) -> (0,0,0) | `9e54d79f29ac47c6` |
| `level0` | то же + `clip=300` | `a4ccdd84a43ca565` |
| `LEVEL10` | 320x200, интерьер | `25e40d77af2f01ba` |

Дополнительно скрипт проверяет:

* новые режимы детерминированы и отличаются от default;
* покрытие кадра при `mip="auto"`/`bilinear` не меняется больше чем на 2%;
* `Baracuda.sob` (текстура с индексом 0) меняется от `alpha_test`;
* синтетические 1x1, 2x2, 4x4 (3 уровня и 1 уровень): отсутствующий мип
  зажимается, `mip=99` не падает, неизвестный фильтр возвращает nearest;
* `alpha_test=True, transparent_index=None` совпадает с default;
* индекс палитры вне диапазона по-прежнему отображается в 0.

## 5. Ограничения и что осталось

* **Нет трилинейного смешивания двух mip-уровней.** Сейчас выбирается один
  уровень и внутри него bilinear. Glide с `pointSampled=FALSE` умеет
  интерполировать между уровнями; это следующий шаг.
* **LOD считается на треугольник, а не на пиксель.** Для сильно
  перспективных треугольников возможна ступенька; формула и bias описаны выше.
* **Dither (`GR_MIPMAP_NEAREST_DITHER`) не воспроизводится.**
* **8-битный палитровый framebuffer и аппаратная палитра не реализованы** —
  рендер по-прежнему RGB.
* **Эвристика «Illegal transparent texture»** (минимум нулей для PICS) не
  копируется: `alpha_test` — явный режим, а не автоматика по имени/архиву.
* **Полигонный флаг `0x2`** не используется как триггер: прямое чтение в
  растеризаторе не найдено, см. §2.3.
* **D3D/софтверный бэкенд** не исследован; все выводы о mip/filter — по
  Glide-пути `gundx.exe`. Формулы драйвера Glide закрыты, поэтому «точная»
  математика LOD остаётся моделью, а не побайтовым паритетом с игрой.
* **C++-рендерер** по-прежнему mip0/nearest; синхронизация с Python — отдельная
  задача (потребует тех же golden-констант и, вероятно, float-оговорок, см.
  `docs/RENDER.md`).

## 6. Воспроизведение

```sh
# Паритет и граничные случаи
python3 tools/parity_mip.py

# Старый путь через CLI (без новых аргументов)
python3 gm.py view "/mnt/d/games/Gunmetal 3dfx/level0.sbw" \
    --game "/mnt/d/games/Gunmetal 3dfx" --size 160 --transform \
    --out /tmp/level0.png

# Новые режимы напрямую из Python
python3 - <<'PY'
from gm import render, sbw
G = "/mnt/d/games/Gunmetal 3dfx"
pool = render.TexturePool(G)
w = sbw.parse_file(G + "/level0.sbw")
ms = render.level_meshes(w, render.import_lightmaps(G + "/level0.sbc"), transform=True)
open("/tmp/level0_mip.png", "wb").write(
    render.render(ms, 320, 200, pool=pool, mip="auto", filter="bilinear"))
PY
```

## C++ renderer

`engine/src/render` mirrors the opt-in modes for a *fixed* level:

```sh
gunmetal --level level0 --mip 1 --filter bilinear --alpha-test \
         --pos 0,0,600 --look 0,0,0 --lm-world \
         --width 320 --height 200 --render out.png
```

`RenderOptions::{mip,mip_auto,bilinear,alpha_test,transparent_index}` sample
the stored `TextureLevel` chain (RGB + palette indices) and skip keyed texels
before the z-write, exactly like `gm/render.py`.  The level0 frame at 320x200
is bit-identical to Python for mip 1 + bilinear + alpha (`2d456b5942c1cac5`)
and for `--mip auto` + bilinear + alpha (`6bcef1ef53276ace`), both pinned in
`gm_tests`; defaults keep the historical mip0/nearest output.  Trilinear
blending and Glide's dither are not implemented.
