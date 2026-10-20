# Спрайтовый блиттер: индексированные `.sbm` в RGB-кадр

Статус: новый C++-модуль `engine/src/render/sprite.*`, в игру не подключён
(`engine/src/main.cpp` не менялся). Блиттер работает с уровнем 0 `.sbm`
(у `sprites/` он всегда единственный), палитрой установки и 24-битным
RGB-кадром с произвольным stride.

Коротко:

* прозрачный индекс (по умолчанию 0) вырезается до записи цвета;
* clip по всем четырём краям, отрицательные `x`/`y` допустимы, wrap нет;
* integer nearest (`blit`) и bilinear (`blit_scaled_bilinear`) с одним и тем же
  `scale`, flip, alpha и additive;
* формулы смешивания повторяют `gm/render.py` (`mix`/`add`, `_clamp`) и
  bilinear-сэмплер оттуда же (wrap по степени двойки, порог покрытия 0.5);
* 16 FNV-1a64 golden'ов (9 синтетических + 7 реальных архивных спрайтов)
  совпадают с временным Python-эталоном; `sprite` проходит CTest за 0.05 с.

## 1. API

Заголовок `engine/src/render/sprite.h`:

```cpp
struct Sprite {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // width*height, top-down
    std::uint32_t flags = 0;          // .sbm header word, not interpreted
};

struct SpriteTarget {
    int width = 0;
    int height = 0;
    int stride = 0;                   // bytes per row, >= width*3
    std::span<std::uint8_t> pixels;   // 24-bit RGB, top-down
};

struct SpriteBlit {
    int x = 0, y = 0;                 // may be negative
    int scale = 1;                    // 1..kSpriteMaxScale (64)
    bool flip_x = false, flip_y = false;
    int alpha = 255;                  // 0..255
    bool additive = false;
    int transparent_index = 0;        // -1 disables the key
};

bool blit(SpriteTarget, std::span<const std::uint8_t> palette,
          const Sprite&, const SpriteBlit&, std::string& error);

bool blit_scaled_bilinear(SpriteTarget, std::span<const std::uint8_t> palette,
                          const Sprite&, const SpriteBlit&, std::string& error);
```

Палитра — 256 RGBA-квадов (тот же layout, что `BmpImage::palette`); альфа
палитры не используется, как и в `sbm_to_rgba`/HUD. Успешный вызов очищает
`error`, ошибка возвращает `false` и заполняет строку.

Валидация (негативы в тесте):

| условие | результат |
|---|---|
| `target.width/height <= 0` | ошибка |
| `stride < width*3` или буфер короче `(height-1)*stride + width*3` | ошибка |
| палитра короче 1024 байт | ошибка |
| `sprite.width/height <= 0` | ошибка |
| `pixels.size() != width*height` | ошибка |
| `scale <= 0`, `> kSpriteMaxScale`, `INT_MAX` | ошибка |
| `alpha < 0` или `> 255` | ошибка |
| `transparent_index` вне `-1..255` | ошибка |
| `alpha == 0` | успешный no-op |
| спрайт целиком вне кадра | успешный no-op |

`kSpriteMaxScale = 64` — политика, а не обход переполнения: все границы
считаются в `int64`, `width*scale` для `.sbm` (максимум 4096) безопасен.

## 2. Геометрия и clip

Прямоугольник спрайта в кадре:

```
left   = x
top    = y
right  = x + width  * scale
bottom = y + height * scale
```

Он пересекается с `[0,width) x [0,height)` кадра; отрицательные координаты
обрезаются, а не заворачиваются. Для пикселя кадра `(px,py)`:

```
dx = px - x            dy = py - y
sx = dx / scale        sy = dy / scale     // целочисленное деление, dx,dy >= 0
if flip_x: sx = width  - 1 - sx
if flip_y: sy = height - 1 - sy
index = pixels[sy * width + sx]
```

Порядок обхода — построчный, слева направо; каждый исходный тексель при
`scale > 1` занимает блок `scale x scale`. Отрицательный `scale` не имеет
смысла «зеркала» — для этого есть `flip_*`; он отвергается.

## 3. Цвет, alpha, additive

Индекс `transparent_index` (если `>= 0`) пропускается до выборки палитры.
Иначе берутся RGB-байты `palette[index*4 + 0..2]`; индекс всегда 0..255,
поэтому выхода за палитру нет.

Смешивание повторяет `gm/render.py::_clamp` (усечение к нулю, затем 0..255) и
формулы `mix`/`add` из `_triangle`, где вес — `w = alpha / 255`:

```
opaque (alpha == 255, !additive):  dst = src
mix:      dst = clamp(dst * (1 - w) + src * w)
additive: dst = clamp(dst + src * w)
```

`alpha == 0` — ранний успешный выход, кадр не трогается. `additive` с
`alpha == 255` — обычное насыщение `min(255, dst+src)` по каждому каналу.
Это точная целочисленная граница той же формулы; порядок операций double
совпадает с Python (`-ffp-contract=off` в `gm_engine`).

## 4. Bilinear

`blit_scaled_bilinear` использует ту же геометрию, но центр пикселя кадра
переводится в непрерывную координату текселя:

```
u = (dx + 0.5) / scale - 0.5
v = (dy + 0.5) / scale - 0.5
if flip_x: u = (width  - 1) - u
if flip_y: v = (height - 1) - v

tx0 = floor(u); ax = u - tx0; tx1 = (tx0 + 1) mod width
ty0 = floor(v); ay = v - ty0; ty1 = (ty0 + 1) mod height

w00 = (1-ax)(1-ay)   w10 = ax(1-ay)
w01 = (1-ax)ay       w11 = ax*ay
```

Четыре соседних текселя выбираются с wrap по маске степени двойки — как в
`gm/render.py` и в Glide (`grTexClampMode(..., 0, 0)`, `docs/MIPMAPS.md` §2.2).
Цвет каждого канала — `sum(channel_k * w_k)`, затем `_clamp`.

Прозрачность при bilinear считается по покрытию, а не по цвету:

```
covered = sum(w_k * (index_k != transparent_index))
if covered <= 0.5: пиксель пропускается
```

Цвета ключевых текселей всё равно входят в смесь — это поведение
`gm/render.py` (`_triangle`, ветка bilinear), а не отдельная эвристика.
Фаза `(dx+0.5)/scale-0.5` — стандартная pixel-center ресемплизация; в
`gm/render.py` та же формула применяется к UV треугольника, поэтому для
спрайта, растянутого на весь экран, результат совпадает по весам.

## 5. Golden'ы

Все хеши — FNV-1a64 по всему буферу кадра, включая padding строк (для
`real_baracuda_pics` stride = `128*3 + 13`). Фон детерминированный:

```
r = (7x + 3y + 11) & 255
g = (2x + 11y + 29) & 255
b = (13x + 5y + 47) & 255
```

Синтетическая палитра: `(i, 255-i, 7i & 255)`.

| случай | параметры | FNV-1a64 |
|---|---|---|
| 2x2 plain | (1,1), scale 1, key off | `cd06f798ac7c9797` |
| 2x2 flip both | (1,1), flip x/y, key off | `645b91aedb80b747` |
| 3x1 scale 2, clip left | (-1,1), key off | `bf6e0ec71d118bee` |
| 2x2 key 0 | (3,2), key 0 | `ad9605eedab6bcb5` |
| 2x2 alpha 128 | (3,2), key off | `74f137c7393d531a` |
| 2x2 additive 255 | (3,2), key off | `f4760823bad8a587` |
| 2x2 bilinear scale 2 | (1,1), key off | `91adcab1f73f4869` |
| 3x3 bilinear key+flip | (-1,0), scale 2, flip x | `7129feaf17f777d6` |
| 2x2 bilinear alpha+add | (1,0), scale 3, alpha 128 | `85c7d50ed324f772` |
| `sprites/AFLM0001` nearest | 64x64, (-10,-20), key 0 | `a51c5882558f4e82` |
| `sprites/BLAM0010` scale 2 + flip x | 128x128, (17,9) | `0e3f29d51a3d7c2e` |
| `sprites/EXPL0001` scale 3 + flip y + alpha 128 | 64x64, (-25,40) | `fa29573b5ea595e6` |
| `PICS/BARACUDA` nearest, padded stride | 128x128, (-17,3), stride 397 | `19120dfff9a726ea` |
| `sprites/AFLM0001` bilinear scale 2 + flip y | 64x64, (5,7) | `980692a7aeb84e98` |
| `sprites/EXPL0001` additive | 64x64, (11,13), scale 2 | `7a2fc62d8ea9b0d2` |
| `sprites/BLAM0001` bilinear scale 3 + flip x + alpha 128 | 128x128, (-4,-6) | `cfb1a077b4b1113b` |

Плюс точные проверки пикселей (без хеша): все четыре угла 2x2 при flips,
блоки scale 1/2/3, обрезка справа/снизу, прозрачные нули, additive clamp
`(255,42,255)`, центр bilinear `(1,253,12)` и порог покрытия 0.375/0.625.

Реальные спрайты читаются из `PICS/SBM.IDX` и `sprites/SBM.IDX` через
`BigFile` + `parse_sbm`; палитра — `installed_palette`. Если игра не
установлена, тест печатает skip и оставляет синтетику.

## 6. Паритет и доверие

`python3 regress.py` (не изменялся): **113/113** быстрых проверок, 83.5 с.
`ctest -R sprite`: **1/1**, 0.05 с, метка `quick`.

Что доказано:

* `.sbm` уровня 0 и общая палитра читаются теми же модулями, что и рендерер;
* nearest-геометрия, clip, flips, integer scale и порядок блоков проверены
  точными пикселями и 16 хешами;
* `mix`/`add`/`_clamp` и bilinear-веса/wrap/порог 0.5 воспроизводят формулы
  `gm/render.py`; Python-эталон дал те же 16 FNV на тех же входах;
* негативы не приводят к OOB/NaN: все размеры и stride проверяются до
  арифметики, индексы палитры — байты, тексельные координаты зажимаются
  wrap'ом.

Что осталось / чему нельзя верить как «движку»:

* **фаза bilinear** (`+0.5` на пиксель) — выбранная модель ресемплинга, а не
  трассировка Glide; для произвольного quad'а двигателя она может отличаться
  на полтекселя;
* **alpha палитры** игнорируется (как в HUD и `sbm_to_rgba`), additive не
  использует material intensity — это параметр вызывающего;
* **mip-уровни** не выбираются: блиттер всегда берёт level 0, как спрайты
  `sprites/` (у них один уровень);
* **LOD, dither, трилинейность Glide** не воспроизводятся;
* **интеграция в игру** не сделана: `main.cpp`/HUD не вызывают новый модуль;
* Python-эталон был временным (`/tmp`), в репозиторий не добавлен — golden'ы
  зашиты константами, Python в runtime не нужен.

## 7. Воспроизведение

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/sandbox --target gm_sprite_tests -j 32
./build/sandbox/engine/tests/gm_sprite_tests
ctest --test-dir build/sandbox -R sprite --output-on-failure
python3 regress.py
```
