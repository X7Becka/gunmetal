# HUD overlay: `hud_overlay.*` (M8.6, срез 2)

* **Статус:** переиспользуемый слой «кадр `.sof`-фильма → готовый RGB-кадр»:
  целочисленный масштаб, обрезка по краям, прозрачный индекс 0, `opacity` и
  статистика покрытия. Заменяет лямбду `main.cpp::overlay_hud`
  (`load_hud_film → installed_palette → hud_player::draw`) как модуль; пиксели
  по-прежнему пишет только `blit_indexed`, часы кадра — `frame_at`.
* **Ветка:** `feat/hudoverlay`, база `master` `84e955c`, дата 2026-10-24.
* **Игра:** только чтение (`Film/*.SOF`, `Film/Kflm0000.bmp`). Временных
  файлов тест не создаёт; ничего не пишется вне процесса.
* **Файлы:** `engine/src/render/hud_overlay.h`,
  `engine/src/render/hud_overlay.cpp`, `engine/tests/test_hud_overlay.cpp`;
  строка `src/render/hud_overlay.cpp` в `engine/CMakeLists.txt`, цель
  `gm_hud_overlay_tests` / тест `hud_overlay` (label `quick`) в
  `engine/tests/CMakeLists.txt`.
* **Не менялись:** `main.cpp`, `config.*`, `render/hud.*`, `hud_player.*`,
  `hud_compose.*`, `ROADMAP.md`.
* **Эталон:** `docs/HUD_CPP.md` (`blit_indexed`, палитра, blend),
  `docs/HUD_PLAYER.md` (`HudFilm`/`frame_at`, golden `AMMO4`),
  `docs/HUD_COMPOSE.md` (политика блита, `(14,125)`).

## 1. API

```cpp
using Palette = std::vector<std::uint8_t>;  // 256 RGBA-квадов, A игнорируется

struct HudOverlayOptions {
    int x = 0;
    int y = 0;
    int scale = 1;
    float opacity = 1.0f;
    bool transparent_zero = true;
    std::uint64_t tick = 0;
};

struct HudOverlayStats {
    int layer_width = 0;        // 128 * scale
    int layer_height = 0;       // 64 * scale
    int drawn_pixels = 0;
    int skipped_transparent = 0;
    int clipped_pixels = 0;
};

bool overlay_glyph_layer(RenderResult& image, const Sof& film, std::size_t frame,
                         const Palette& palette, const HudOverlayOptions& options,
                         HudOverlayStats* stats, std::string* error);

bool overlay_film(RenderResult& image, const HudFilm& film, const Palette& palette,
                  const HudOverlayOptions& options, HudOverlayStats* stats,
                  std::string* error);

struct HudOverlayLayer { const HudFilm* film = nullptr; HudOverlayOptions options{}; };

bool compose_hud_overlay(RenderResult& image,
                         std::span<const HudOverlayLayer> layers,
                         const Palette& palette, HudOverlayStats* stats,
                         std::string* error);
```

* `overlay_glyph_layer` — примитив: уже декодированный кадр (номер тайла) на
  кадр. Вызывает существующий `blit_indexed`; **своего** цикла по пикселям в
  модуле нет, только геометрическая классификация для статистики.
* `overlay_film` — то же с выбором кадра через `frame_at(film, tick)`
  (`start_frame`/`loop`/`count`/`fps` берутся из `HudFilm`, см.
  `docs/HUD_PLAYER.md` §2). Это замена `main.cpp::overlay_hud`: тот делает
  ровно `draw(..., transparent=true, scale, opacity, tick)`.
* `compose_hud_overlay` — упорядоченный стек готовых фильмов (поздний слой
  поверх раннего). Это **не** конвейер `.sml`-экранов — он уже есть в
  `render/hud_compose.h`; здесь вызывающий сам решает, какие фильмы и где.
  Все слои проверяются до первого блита, пустой стек — успешный no-op.
* `error == nullptr` и `stats == nullptr` допустимы (тихий вызов для viewer'а).
  При успехе `*error` очищается; при отказе буфер не изменён, `*stats`
  обнулена, текст начинается с `hud_overlay: ` (вложенный `hud: ...`
  сохраняется).

### 1.1. Контракт опций

| поле | значение | отказ |
|---|---|---|
| `x`, `y` | любой `int`; отрицательные и далёкие — обрезка, не заворот | — (слой вне кадра — успешный no-op) |
| `scale` | 1..4 (`kHudMaxScale`), nearest, блок `scale × scale` | `hud_overlay: scale 0 outside 1..4` |
| `opacity` | 0..1 **включительно** | `opacity 1.5 outside 0..1`, `opacity is not finite` |
| `transparent_zero` | не рисовать индекс 0 (фон `.sof`) | — |
| `tick` | счётчик 91 Гц | переполнение `tick * fps` (64 бита) — отказ `frame_at` |

`opacity` здесь строже, чем в `blit_indexed`: блиттер клампит конечные
значения (`docs/HUD_CPP.md` §2), а overlay считает `1.5` ошибкой вызывающего.
`0` — валидное значение: буфер не меняется, покрытие всё равно считается.

### 1.2. Статистика

Каждая масштабированная клетка слоя классифицируется ровно один раз, в том же
порядке, что и в `blit_indexed` (сначала ключ, потом границы):

* `transparent_zero && index == 0` → `skipped_transparent += scale²`;
* иначе пересечение блока `scale × scale` с кадром → `drawn += nx*ny`,
  остаток блока → `clipped`.

Поэтому **всегда**

```
drawn_pixels + skipped_transparent + clipped_pixels == layer_width * layer_height
```

`drawn_pixels` — покрытие, т.е. пиксели, которые блиттер пишет. Единственное
исключение — `opacity == 0`: блиттер выходит до записи, но покрытие
сообщается, чтобы инвариант площади держался (тест это фиксирует отдельно).

## 2. Числа

Сборка и прогон:

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx -DGM_WITH_SDL3=OFF
cmake --build build --target gm_hud_overlay_tests -j 4
ctest --test-dir build -R '^hud_overlay$' --output-on-failure    # ~0.7 s
python3 regress.py --mirror --json                               # 115/115
```

### 2.1. Синтетика (без игры)

Кадр 320×200, палитра `i → (i, 255-i, 7i)`; тайл-шаблон 128×64: 32 левых
столбца — индекс 0 (2048 клеток), дальше индекс 7 сверху и 9 снизу.

```
pattern at (0,0) scale=1, fill=(11,22,33):
  layer=128x64 drawn=6144 skipped=2048 clipped=0 changed=6144 area=8192
pattern at (0,0) scale=2:
  layer=256x128 drawn=24576 skipped=8192 clipped=0 changed=24576 area=32768
pattern at (-8,192) scale=1:
  drawn=768 skipped=2048 clipped=5376 strays=0   (768 = 96 столбцов × 8 строк)
opacity=0.5 (bg=(0,0,0)):
  (40,5)  src=(7,248,49) got=(4,124,25)
  (60,50) src=(9,246,63) got=(5,123,32)
opacity=0: буфер не изменён, coverage=6144
transparent_zero: true changed=6144 skipped=2048 | false changed=8192 skipped=0
compose 2 слоя (scale 1 + scale 2): drawn=22528 skipped=10240 clipped=8192
  layer=256x128, пиксели == два последовательных overlay_film
```

`INT_MIN`/`INT_MAX` в `x`/`y` — успешный no-op (`drawn=0`, инвариант 8192),
арифметика в `int64` не переполняется.

### 2.2. Поставка: `AMMO4.SOF` (ammo.fdx:4, 131 кадр @ 24 fps, `start_frame=1`)

Кадр 320×200, палитра `installed_palette`, кадр по тику 0 = тайл 1.

```
(0,0) scale=1 fill=(7,9,11): layer=128x64 drawn=8189 skipped=3 clipped=0
                             changed=8189 area=8192
(0,0) scale=1 transparent_zero=false: changed=8192 skipped=0 (3 px индекса 0)
(0,0) scale=2:               layer=256x128 drawn=32756 skipped=12 clipped=0
                             changed=32756 area=32768
(-8,192) scale=1:            drawn=960 skipped=3 clipped=7229 strays=0
opacity=0.5 (bg=0):          touched=8167, formula_mismatches=0
                             (65,8) src=(87,87,87) got=(44,44,44)
```

`changed == drawn` на `scale=1` означает, что все нарисованные клетки реально
изменили кадр (фон `7,9,11` не совпал ни с одним цветом кадра). Индекс 0 в
поставленных HUD-фильмах почти не встречается (у `AMMO4` кадр 1 — 3 пикселя,
кадры 25/49 — ноль), поэтому контраст «ключ/без ключа» на поставке всего
3 px; сильная проверка прозрачности — синтетическая (2048 клеток).

### 2.3. Паритет с текущим `overlay_hud`

`main.cpp::overlay_hud` = `hud_player::draw(..., transparent=true, scale,
opacity, tick)` с `installed_palette`. Тест сравнивает **весь буфер**
`overlay_film` с `draw` побайтово (не только хеш):

| tick | (x,y) | scale | opacity | key | drawn | clipped | FNV-1a64 |
|---|---|---|---|---|---|---|---|
| 0 | (14,125) | 1 | 1.0 | 1 | 8189 | 0 | `2e32d52ce8678d4b` |
| 91 | (14,125) | 2 | 0.75 | 1 | 19200 | 13568 | `8dfa7083032f25e5` |
| 182 | (0,0) | 1 | 0.5 | 1 | 8192 | 0 | `c3e5e9076f6c45e7` |
| 0 | (14,125) | 4 | 1.0 | 0 | 22950 | 108122 | `8b3e71df012897f3` |

Стек `AMMO4 + WEAPON8` тоже побайтово равен двум последовательным
`overlay_film` (`drawn=16381 skipped=3 clipped=0`,
FNV `8dcc07251c8e8e4e`).

Две закреплённые константы:

* `2e32d52ce8678d4b` — путь **с ключом 0** (то, что делает CLI
  `--render --hud`), кадр 320×200, `(14,125)`, tick 0;
* `aaf521e923f31099` — golden `docs/HUD_PLAYER.md` §4, полученный **без**
  прозрачности и с палитрой `Film/Kflm0000.bmp`. `overlay_film` его
  воспроизводит (`transparent_zero=false`, та же палитра).

## 3. Доказано

* Паритет с существующим поведением: `overlay_film` и `hud_player::draw`
  дают побайтово одинаковый кадр на поставке для 4 комбинаций
  tick/scale/opacity/ключа, включая позицию оверлея `(14,125)`; 4 FNV выше
  воспроизводятся, `2e32…`/`aaf5…` закреплены.
* Числа `drawn/skipped/clipped` — не оценка: инвариант площади проверяется
  для synthetic (scale 1, scale 2, обрезка, полностью вне кадра, INT_MIN/MAX,
  стек) и для поставки (scale 1/2, обрезка), сумма всегда равна
  `layer_width * layer_height`.
* Обрезка не пишет вне видимого прямоугольника: тест обходит **весь** кадр и
  считает «чужие» изменения (`strays=0`) для `(-8,192)` и для слоя вне кадра.
* `opacity` реализован формулой `floor(dst + (src-dst)*a + 0.5)` — проверено
  на всех покрытых пикселях для 0.25/0.5/0.75 (synthetic, 6144 px × 3 канала)
  и для 0.5 на поставке (`touched=8167`, 0 расхождений).
* Нулевой индекс прозрачен: synthetic 2048 клеток, при `transparent_zero=false`
  меняется 8192 px вместо 6144, `skipped=0`; на поставке — 8192 против 8189
  (у кадра 1 всего 3 пикселя индекса 0).
* Детерминизм: два прогона с одинаковыми входами (synthetic и поставка) дают
  одинаковые байты и одинаковую статистику.
* Отказы не портят кадр: пустой фильм, `count=0`, `count > tiles`, `scale`
  0/5/-1/INT_MIN, `opacity` -0.1/1.5/100/NaN/+inf, палитра на 255 квадов,
  пустой target, короткий буфер, `tick=UINT64_MAX`, кадр вне диапазона,
  битый слой стека, `film=nullptr` — везде `false`, буфер побайтово тот же.
* `regress.py --mirror --json` — 115/115; `hud_overlay` в CTest — `quick`,
  ~0.7 с, 261 проверка, 0 падений.

### 3.1. Наблюдение по палитрам (уточнение к `docs/HUD_CPP.md` §2)

`installed_palette` (GAMEPAL из `FRONTEND/BMP.IDX`) и палитра
`Film/Kflm0000.bmp` различаются **только записью 0**: `(0,0,0)` против
`(0,255,255)` (неиспользованный слот дампа). На пути с ключом 0 это не видно,
поэтому `2e32…` одинаков для обеих таблиц, а `aaf5…` (без ключа) требует
именно `Kflm0000`. Это объясняет, почему в `docs/HUD_PLAYER.md` golden снят
без прозрачности.

## 4. Ограничения

* **Подключено (срез 3).** `main.cpp::overlay_hud` вызывает
  `overlay_film()` с теми же опциями (`--hud/--hud-x/--hud-y/--hud-scale/`
  `--hud-opacity/--hud-tick`); кадр `--render --level level0` без HUD
  побайтово тот же (md5 `ae2c8e98…` до/после), с `--hud AMMO4 --hud-x 14
  --hud-y 125` — PNG 1 024 543 Б.
  `overlay_hud` доказан тестом, но сам `overlay_hud` пока оставлен как есть
  (правка `main.cpp` вне разрешённого списка файлов).
* **Нет `.sml`-разметки**: позиции/семейства — `hud_compose.*`; overlay рисует
  то, что дал вызывающий.
* **Нет выбора палитры и fade**: `Palette` — аргумент; `hud_lut.*`/затемнение
  экрана не применяются.
* **Нет движкового шага `ms = 91 // fps`**: кадр выбирает `frame_at` с
  рациональной формулой (`docs/HUD_PLAYER.md` §2); расхождение для `fps`,
  не кратного 91, остаётся как и в `hud_player`.
* **`drawn_pixels` при `opacity == 0`** — покрытие, а не факт записи
  (блиттер выходит раньше); задокументировано, чтобы инвариант площади был
  однозначным.
* **Ключ только индекс 0**: другой прозрачный индекс не поддержан (как и в
  `hud_player::draw`); при необходимости поле добавляется в
  `HudOverlayOptions`.
* `drawn/skipped/clipped` — `int`; один слой ≤ 131072 клеток
  (`8192 * 4²`), стек ограничен числом слоёв вызывающего.
* `compose_hud_overlay` валидирует всё до первого блита, но если
  `blit_indexed` всё же откажет на слое N, ранние слои уже нарисованы; это
  недостижимо после проверок, но правило зафиксировано (как в
  `docs/HUD_COMPOSE.md` §7).

## 5. Проверки

```sh
./build/engine/tests/gm_hud_overlay_tests                       # 261 checks, 0 failed
ctest --test-dir build -R '^hud_overlay$' --output-on-failure   # quick, ~0.7 s
python3 regress.py --mirror --json                             # 115/115
```

Без установки игры половина поставки печатает
`hud_overlay: game not installed, shipped cases skipped`, а вся синтетика,
негативы и инварианты выполняются всегда (тест не требует игры).
