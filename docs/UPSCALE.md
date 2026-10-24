# Целочисленный апскейл и палитровый fade (`engine/src/render/upscale.h`)

Срез 2 пункта `M8.5`: кадр фронтенда **320×200** (пагер — `ui/pager_render.h`,
меню — `ui/menu_render.h`) нужно показывать крупнее, а fade применять таблицей,
а не формулой на каждый пиксель. Модуль делает ровно две вещи, обе без игры и
без окна:

* `upscale_nearest` — целочисленный nearest 1..8: каждый исходный пиксель
  повторяется блоком `scale×scale`, ни один байт цвета не меняется;
* `fade_rgba` — затемнение готового кадра по кривой движка (`0x682f90`) через
  LUT из 256 значений на канал, альфа сохраняется.

Код: `engine/src/render/upscale.h/.cpp`; тест — `engine/tests/test_upscale.cpp`
(CTest `upscale`, label `quick`, 0.53 с). CLI подключён: `--png-scale N`
для `--menu --png` и `--pager --png` (§6).

Что здесь доказано, что модель:

| Утверждение | Статус |
|---|---|
| nearest 1..8, побитовое повторение пикселя | доказано тестом (каждая точка, scale 1/2/3/8) |
| fade при `level = max_level` — тождество | по построению; дамп движка на уровне 11 — тождество по цвету 256/256 (§3.2) |
| fade при `level = 0` — чёрный (альфа сохраняется) | по построению; дамп на уровне 0 отправляет 236/256 индексов в чёрный слот (§3.2) |
| fade в промежуточных уровнях | **модель**: `(v·level + max/2)/max`, точная переквантовка движка не разобрана (§3.3) |

## 1. API

```cpp
#include "render/upscale.h"

// 1) апскейл: src -> out, w*s × h*s, 0xRRGGBBAA
std::vector<std::uint32_t> rgba;      // кадр 320×200 от render_page/render_screen
int width = 0, height = 0;            // 320 / 200
gm::UpscaleStats stats;
std::string error;
if (!gm::upscale_nearest(rgba.data(), width, height, 4, big, &error, &stats)) {
    // error: текст причины, big пуст
}
// big.size() == 64000*16, big[(y*4+dy)*1280 + x*4+dx] == rgba[y*320 + x]

// 2) fade: уровень 0..max_level, альфа сохраняется
if (!gm::fade_rgba(big, 5, 11, &error)) { /* error: max_level < 1 */ }

// 3) таблица отдельно (тест/будущий CLI), 256 значений на канал
std::uint8_t lut[gm::kFadeLutSize];
gm::fade_lut(5, 11, lut, &error);     // lut[v] = (v*5 + 5)/11
```

| Функция | Возврат | Ошибки (false + текст) |
|---|---|---|
| `upscale_nearest(const uint32_t* src, int w, int h, int scale, vector<uint32_t>& out, string* error = nullptr, UpscaleStats* stats = nullptr)` | `out` = `w·s × h·s` | `src == nullptr`; `w`/`h` ≤ 0; `scale` вне `1..8`; `w·h·s²` не влезает в `size_t` |
| `fade_rgba(vector<uint32_t>& rgba, int level, int max_level, string* error = nullptr)` | буфер затемнён на месте | `max_level < 1` (кривой нет); пустой буфер — успешный no-op |
| `fade_lut(int level, int max_level, uint8_t (&lut)[256], string* error = nullptr)` | 256 значений | `max_level < 1` |

Константы: `kUpscaleMaxScale = 8` (320×200 при 8 — 2560×1600, 4 096 000
пикселей; ограничение политики, а не арифметики — проверка размера 64-битная),
`kFadeLutSize = 256`, `UpscaleStats{scale, pixels_in, pixels_out}`.

Решения по границам (то, что в задаче допускалось «кламп или отказ»):

* **`scale` вне 1..8 — отказ** с текстом, `out` очищается: молчаливый кламп
  превратил бы опечатку `--png-scale 80` в 8-кратный кадр, который никто не
  просил. `out` очищается и при любой другой ошибке, чтобы вызывающий никогда
  не показал старый кадр как результат.
* **`level` вне `0..max_level` — кламп** (`<0` → 0, `>max` → тождество), как в
  пагере: `kPagerFadeLevels = 16`, `kPagerFadeMax = 11`, CLI принимает 0..15 и
  клампит к 11. `max_level < 1` — отказ (делить не на что); при отказе буфер
  **не трогается**.
* **пустой буфер**: `upscale_nearest` — отказ (`w`/`h` не положительны),
  `fade_rgba` — успешный no-op.

## 2. Nearest: геометрия и почему без фильтра

`upscale_nearest` — это тот же приём, что `HudLayer::scale` в
[`render/hud.h`](HUD_CPP.md) §2 и `blit` в [`render/sprite.h`](SPRITE.md):
источник читается, но не семплируется — каждый тексель становится квадратным
блоком, поэтому цвет и альфа переносятся побитово (`memcpy`-семантика), а не
через интерполяцию. В отличие от `render/scale.h` (`blit_scaled`, RGB24 →
XRGB8888 с letterbox и опциональным bilinear для окна), здесь нет ни
масштабирования под размер окна, ни фильтра, ни полей: результат — чистая
функция `(src, w, h, scale)`, и его можно сравнивать побайтово.

Реализация строит строку результата (`scale` копий каждого пикселя через
`std::fill_n`) и размножает её `scale-1` раз через `std::copy`, поэтому
внутренний цикл идёт по исходным пикселям, а не по приёмным. Размеры:

| scale | 320×200 → | пикселей | байт RGBA |
|---:|---|---:|---:|
| 1 | 320×200 | 64 000 | 256 000 |
| 2 | 640×400 | 256 000 | 1 024 000 |
| 4 | 1280×800 | 1 024 000 | 4 096 000 |
| 8 | 2560×1600 | 4 096 000 | 16 384 000 |

Проверка в тесте — не «размеры совпали», а каждая точка:
`out[(y·s+dy)·w·s + x·s+dx] == src[y·w+x]` для всех `x, y, dx, dy`. Сетка 3×2
из шести уникальных цветов (включая `alpha 0/128/200`, чтобы альфа не
«нормализовалась») при scale 1/2/3/8 даёт 0 несовпадений (468 приёмных
пикселей); реальный кадр 320×200 (каждый пиксель свой) при 1/2/4/8 — тоже 0,
это ещё 5 378 000 проверенных приёмных пикселей.

## 3. Fade: закон, границы, сверка с дампом `0x682f90`

### 3.1 Модель

Кривая движка: 16 таблиц по 256 байт по `0x682f90`, строятся на старте
`0x48f970` → `0x483290(dst, k/11, 1)` (HUD_AUDIT §2.7), блит полосы переводит
индекс через `LUT[k]` (`FUN_00a1b510(..., &DAT_00682f90 + k*0x100)`). Фактор —
`k/11`, уровень 11 — тождество, уровень 0 — чёрный.

В C++ это та же модель, что в пагере ([`docs/PAGER_RENDER.md`](PAGER_RENDER.md)
§2.5), обобщённая на произвольный `max_level` (в движке и пагере он 11):

```
fade(v, level, max) = (v * clamp(level, 0, max) + max/2) / max     // целочисленно, v = 0..255
```

`max/2` — целочисленное, т.е. округление half-up; при `max = 11` это ровно
`(v*k + 5)/11` — то же целочисленное выражение, что `fade_channel` в
`engine/src/ui/pager_render.cpp`. Тест закрепляет 256/256 значений при
`max_level = 11` против независимой Python-реализации того же закона (§3.4),
поэтому путь пагера и путь модуля не могут разойтись молча (сам `fade_channel`
статичен и снаружи не вызывается — сверка идёт по выражению и по эталону).

Почему LUT, а не формула на пиксель: таблица строится один раз на кадр
(256 умножений/делений), затем на пиксель приходится три чтения из массива
(192 000 чтений на кадр 320×200) вместо 192 000 умножений и делений. Замер —
§4, выигрыш в Debug 1.5..1.7× (11.9 против 20.1 нс/пиксель); результат обеих
реализаций обязан совпасть побайтово, это проверяется в том же тесте.

### 3.2 Что даёт дамп `hud_lut.bin` (данные, не модель)

В кэше есть дамп самих таблиц: `~/.cache/gunmetal-re/hud_lut.bin`, 4096 байт =
16×256, снят `tools/win/dump_lut.lua` с адреса `0x682F90` (регрессия
`render / hud fade level round trip` использует тот же файл). Таблица —
**индекс → индекс**, поэтому сравнивать её с канальным LUT можно только в
пространстве цвета: `rgb[lut[i]]` против `rgb[i]`, прогнанного через модель.
Палитра — `FRONTEND/GAMEPAL` (`gm/palette.py::installed`).

Команда (воспроизводит таблицу ниже):

```sh
python3 - <<'EOF'
import os, sys
sys.path.insert(0, ".")
from gm import palette
game = os.path.expanduser("~/.cache/gunmetal-re/game")
pal = palette.installed(game)
rgb = [(pal[i*4], pal[i*4+1], pal[i*4+2]) for i in range(256)]
dump = open(os.path.expanduser("~/.cache/gunmetal-re/hud_lut.bin"), "rb").read()
lut = lambda k, m=11: [(c*k + m//2)//m for c in range(256)]
print("lvl colour-identity meanCErr maxCErr")
for level in range(16):
    table = dump[level*256:(level+1)*256]
    model = lut(min(level, 11))
    same = sum(1 for i in range(256) if rgb[table[i]] == rgb[i])
    errs = [abs(rgb[table[i]][c] - model[rgb[i][c]]) for i in range(256) for c in range(3)]
    print(f"{level:3d} {same:15d} {sum(errs)/len(errs):8.2f} {max(errs):7d}")
EOF
```

| уровень | цветовых тождеств из 256 | средняя ошибка канала vs модель | макс. ошибка |
|---:|---:|---:|---:|
| 0 | 22 | 8.40 | 255 |
| 1 | 22 | 10.29 | 232 |
| 2 | 22 | 10.68 | 209 |
| 3 | 22 | 10.49 | 185 |
| 4 | 22 | 10.18 | 162 |
| 5 | 22 | 10.06 | 139 |
| 6 | 22 | 9.63 | 116 |
| 7 | 23 | 9.16 | 93 |
| 8 | 24 | 8.57 | 70 |
| 9 | 34 | 7.85 | 46 |
| 10 | 70 | 6.32 | 27 |
| **11** | **256** | **0.00** | **0** |
| 12 | 132 | 7.01 | 40 |
| 13 | 73 | 15.14 | 60 |
| 14 | 48 | 22.46 | 84 |
| 15 | 45 | 27.90 | 104 |

Выводы:

* **Уровень 11 — точное тождество по цвету: 256/256.** По индексам 254/256
  (`0 → 1`, оба чёрные) — это ровно то «254/256 совпадающих записей», что уже
  записано в HUD_AUDIT §2.7. Значит верхняя граница кривой в модели
  (`level == max_level` → кадр байт в байт) согласуется с движком, а не только
  с нашей формулой.
* **Уровень 0 — 236/256 индексов уходят в слот 1 (`GAMEPAL[1] = (0,0,0)`),
  20 остаются тождественными.** Эти 20 — ровно те слоты, где `FLAG/naUSA.BMP`
  отличается от `FRONTEND/GAMEPAL` (173/174/175, 191, 240..255 — докстринг
  `gm/palette.py`); плюс индексы 0 и 1, которые и так чёрные, получается 22
  «цветовых тождества». Почему таблица их не трогает — **гипотеза**: она
  строится не по всем 256 слотам, а по списку рамп (вне рамп запись остаётся
  как была). Для канальной модели это неважно: `v=0 → 0` при любом уровне 0.
* **Уровни 12..15 не «почти тождественны»**: тождеств по цвету 132/73/48/45, и
  ошибка растёт с уровнем. Это опровергает формулировку
  `ui/pager_render.h` («12..15 behave like 11 (the tables are near-identity)») и
  §2.5 PAGER_RENDER: фактор там `k/11 > 1`, т.е. таблицы движка *осветляют*.
  Модель сознательно оставляет кламп к `max_level` (как пагер), потому что
  RGBA-путь не воспроизводит переквантовку по палитре; расхождение
  зафиксировано здесь, файлы пагера срез 2 не правит.

### 3.3 Промежуточные уровни — модель, а не паритет

Средняя ошибка канала 6..11 при максимуме 27..139 (уровень 5: средняя 10.06,
максимум 139) означает, что таблица движка делает не линейное умножение
канала, а переквантовку **индекса**: например, при уровне 10 индекс 2
`(23,23,23)` уходит в 144 `(19,15,15)`, хотя ближайший к `23·10/11 = 21` цвет —
сам индекс 2. Как именно `0x483290` раскладывает цвет по рампам палитры, не
разобрано (в HUD_AUDIT §2.9 это и записано: «формула не разобрана — берём
дамп»), поэтому:

* границы кривой — **доказаны** (проверены дампом в пространстве цвета);
* промежуточные уровни — **модель**; сравнивать наш RGBA-кадр с оригиналом на
  уровне 5 побайтово нельзя, только «по яркости»;
* палитровый путь (индекс → LUT индексов → палитра) в этом модуле не
  реализован (§5) — там как раз нужен дамп `hud_lut.bin`, а не формула.

### 3.4 Эталонные числа для теста (Python)

256-элементные таблицы для уровней 1, 5, 8, 11 сгенерированы независимой
реализацией на Python (не тем же C++-кодом) и вшиты в тест:

```sh
python3 - <<'EOF'
for level in (1, 5, 8, 11):
    print(level, [(c*level + 11//2)//11 for c in range(256)])
EOF
```

Тест сверяет **все 256** значений каждого уровня (не выборку): 4×256 = 1024
сравнения, все совпадают. Дополнительно закреплены концы: `lut[0] = lut[255] =
0` при уровне 0 и 256/256 тождеств при 11.

## 4. Числа теста (`upscale`, label `quick`)

Прогон: `cmake --build build/sandbox --target gm_upscale_tests -j 4` и
`ctest --test-dir build/sandbox -R '^upscale$'` — 1/1 passed, 0.53 с
(Debug, Zig 0.17). Вывод теста (сокращённо, но числа дословно):

```
nearest 3x2 scale 1 -> 3x2,   pixels 6 -> 6,      mismatched destination pixels 0
nearest 3x2 scale 2 -> 6x4,   pixels 6 -> 24,     mismatched destination pixels 0
nearest 3x2 scale 3 -> 9x6,   pixels 6 -> 54,     mismatched destination pixels 0
nearest 3x2 scale 8 -> 24x16, pixels 6 -> 384,    mismatched destination pixels 0
nearest canvas 320x200 scale 1 -> 320x200,   pixels 64000 -> 64000   (bad 0)
nearest canvas 320x200 scale 2 -> 640x400,   pixels 64000 -> 256000  (bad 0)
nearest canvas 320x200 scale 4 -> 1280x800,  pixels 64000 -> 1024000 (bad 0)
nearest canvas 320x200 scale 8 -> 2560x1600, pixels 64000 -> 4096000 (bad 0)
fade level 0/11: nonzero RGB pixels 0/256, alpha preserved 256/256 (0xc83778c8 -> 0x000000c8)
fade level 11/11: 256/256 pixels byte-identical (0xc83778c8 -> 0xc83778c8)
fade clamp: level 15 -> identity, level -7 -> black
fade level 5/11: deterministic on 64000 px, channels that grew 0, 0xc83778c8 -> 0x5b1937c8
fade lut level 5/11: monotone (decreasing 0), no channel grew (0), 0->0 128->58 255->116
fade lut level 1/11: 256/256 entries equal the Python reference
fade lut level 5/11: 256/256 entries equal the Python reference
fade lut level 8/11: 256/256 entries equal the Python reference
fade lut level 11/11: 256/256 entries equal the Python reference
fade lut 0/11: 0 x 256 (black); 11/11: 256/256 identity
error scale 0: "upscale: scale 0 outside 1..8"
error scale 9: "upscale: scale 9 outside 1..8"
error source 0x1: "upscale: source size 0x1 is not positive"
error null source: "upscale: null source"
error overflow 2^30x2^30 at scale 8: "upscale: 1073741824x1073741824 at scale 8 overflows the buffer"
error fade max_level 0: "upscale: fade max_level 0 must be >= 1" (buffer untouched)
empty buffer: upscale refused, fade accepted as a no-op
perf upscale 320x200 x8 -> 2560x1600 px, 5 runs in 172 ms (119.1 Mpx/s)
perf fade 64000 px level 7/11: LUT 76360 us, per-channel 128753 us
  (100 runs, 11.9 vs 20.1 ns/px)
perf fade: LUT equal to the per-channel formula on 64000 px
upscale: ok
```

Производительность (Debug, без оптимизаций; Release будет быстрее):
апскейл 320×200 в 8× — 4 096 000 пикселей за ~34 мс на прогон
(119.1 Mpx/s), fade 64 000 пикселей — 11.9 нс/пиксель (LUT) против
20.1 нс/пиксель (формула на канал). Числа плавают от прогона к прогону:
измерено 4 запуска — апскейл 105.6..119.1 Mpx/s, LUT 11.9..13.5 нс/px,
формула 20.1..20.6 нс/px, т.е. выигрыш LUT 1.5..1.7×. Обе реализации дают
побайтово один кадр (`CHECK` после первого применения). Тест не падает по
времени: он укладывается в ~0.5 с.

## 5. Ограничения (сознательно)

* **Нет bilinear/фильтрации.** Nearest — единственный режим: у апскейла нет
  параметра фильтра, как и у `HudLayer::scale`. Сглаженный/letterbox-путь для
  окна остаётся в `render/scale.h` (там RGB24 и размер окна).
* **Нет палитрового пути 8 bpp.** Модуль знает только `0xRRGGBBAA`. Индексный
  fade (`hud_lut.bin`, индекс → индекс) и апскейл индексного буфера — отдельная
  задача; здесь его нет, поэтому промежуточные уровни и не могут совпасть с
  движком побайтово (§3.3).
* **Альфа не затемняется** — сохраняется точно (`colour & 0xFF`). В движке
  альфы в кадре нет (8 bpp), так что «правильного» поведения для неё не
  существует; выбрано наименее разрушительное.
* **Нет дизеринга/интерлейса 320×200** оригинала и нет привязки к окну:
  масштаб 320×200 → 640×400/2560×1600 делает вызывающий.
* **CLI:** `--png-scale N` подключён (см. §6); fade — тем же флагом
  `--fade N` (0..15, max 11) и для пагера, и для меню, всегда до зума.
  Окно (`platform/window.cpp`) по-прежнему не использует модуль: там нужен
  произвольный размер и letterbox (`render/scale.h`).
* `max_level` произвольный, но уровни **выше** него клампятся к тождеству —
  поведение уровней 12..15 движка (осветление) модель не воспроизводит (§3.2).

## 6. CLI: `--png-scale N` (срез 3, 2026-10-24)

Флаг добавлен в `engine/src/core/config.*` (1..8, по умолчанию 1 — прежнее
поведение не меняется) и применяется в `engine/src/main.cpp` к **готовому**
кадру фронтенда:

* `--menu [--menu-screen ID] [--menu-keys ...] --png OUT --png-scale N` —
  после `render_screen`;
* `--pager FILE [--page N] [--background] [--fade K] --png OUT --png-scale N` —
  после `render_page`, то есть fade идёт по кадру 320×200, а зум — последним
  (в движке LUT работает по экранному буферу, апскейл — уже презентация).

Реальные прогоны (2026-10-24, установка v1.36):

```
--menu --png x.png --png-scale 3
  scale: x3 960x600 (64000 -> 576000 pixels)
  render: MAIN MENU 960x600 items=7 drawn=7 ink=638 missing=0
  png: x.png (2304843 bytes), IHDR 960x600

--pager SML/brief/level1.sml --png y.png --png-scale 2 --background --fade 9
  render: page 1/2 320x200 ... ink=7214 background=64000
  scale: x2 640x400 (64000 -> 256000 pixels)
  png: y.png (1024543 bytes), IHDR 640x400
```

Пиксельная проверка PNG через `gm/png.py` (не только размер):
кадр `--menu-screen 10` при `--png-scale 3` — **0 несовпадений из 576 000**
пикселей: каждый блок 3×3 побайтово равен исходному пикселю 320×200
(включая альфу).

Там же проверены концы кривой на CLI: `--fade 0` → **0** не-чёрных пикселей
(64 000), `--fade 11` → **0** отличий от кадра без fade, альфа 255 у всех
64 000. Порядок «fade → зум» подтверждён тем, что `--fade 5 --png-scale 2`
даёт блоки 2×2 из уже затемнённых пикселей.

Заодно исправлена ошибка: `--menu --png` **без** `--menu-keys` печатал экран и
возвращался до записи PNG; теперь картинка пишется в обоих режимах.
`--fade N` работает и для меню (`cli_menu_png_fade`: `fade: level 5 (max 11)`),
порядок — fade, затем зум (`cli_menu_png_fade_scale`).

CTest: `cli_pager_png_scale`, `cli_pager_png_scale_bad`, `cli_menu_png_plain`,
`cli_menu_png_scale`, `cli_menu_png_fade`, `cli_menu_png_fade_scale`
(label `quick`), все `cli_*png*` — зелёные.

Осталось: окно/произвольный размер — `render/scale.h`, отдельный срез.

## 7. Проверка (выполненные команды)

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox --target gm_upscale_tests -j 4
ctest --test-dir build/sandbox -R '^upscale$' --output-on-failure   # 1/1 passed, 0.53 s
./build/sandbox/engine/tests/gm_upscale_tests                       # числа §4
python3 regress.py --mirror --json                                  # 115/115 passed, 60.6 s
```

Полный `ctest` не запускался намеренно (его прогоняет основной агент при
интеграции): срез собирает только свою цель `gm_upscale_tests`.
