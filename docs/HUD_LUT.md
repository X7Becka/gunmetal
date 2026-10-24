# HUD fade LUT: точные таблицы `0x682f90` (`M8.5`, срез 7)

Статус: **срез 7** (2026-10-24). Модуль `engine/src/render/hud_lut.h` +
`.cpp`, тест `hud_lut` (CTest, label `quick`), этот документ. Срез 3 добавил
чтение дампа, **срез 7 — построение таблиц из палитры** (`build_hud_lut`,
формула `0x483290` разобрана в `docs/LUT_BUILDER.md`; §2b). Ничего из
существующего не менялось, кроме `hud_lut.*`, теста и этого документа.

Зачем: полоса кокпита блитится не «как есть», а через таблицы затемнения
`0x682f90` (`0x491f10`..`0x491f2d`, `docs/HUD_AUDIT.md` §2.7). Таблицы
строятся на старте (`0x48f970` → `0x483290(dst, k/11, 1)`), поэтому в поставке
их нет — единственный точный источник это дамп
`~/.cache/gunmetal-re/hud_lut.bin` (4096 Б, `tools/win/dump_lut.lua`,
`docs/HUD_AUDIT.md` §2.8). Дамп теперь **проверка**, а не источник: те же
4096 Б строит C++ (§2b). `render/upscale.h` (`fade_rgba`) остаётся канальной
**моделью** и не трогается.

## 1. API

```cpp
namespace gm {

inline constexpr int      kHudLutLevels        = 16;    // k = 0..15
inline constexpr int      kHudLutEntries       = 256;   // палитровый индекс
inline constexpr std::size_t kHudLutBytes      = 4096;  // 16 x 256, ровно
inline constexpr int      kHudLutMaxLevel      = 15;    // кламп движка
inline constexpr int      kHudLutIdentityLevel = 11;    // k/11 == 1
inline constexpr std::size_t kHudLutPaletteBytes = 1024; // 256 quads RGBA

struct HudLut {                                     // levels[k][i] = индекс
    std::array<std::array<std::uint8_t, 256>, 16> levels{};
    bool operator==(const HudLut&) const = default;
};

struct HudLutStats { std::string source; std::size_t bytes;
                     int levels; int entries; };

struct HudLutLevel { int identity; int colour_identity;  // -1 = не измерялось
                     int white; int min; int max; };

bool  load_hud_lut(const std::filesystem::path&, HudLut&,
                   std::string* error = nullptr, HudLutStats* stats = nullptr);
std::uint8_t map(const HudLut&, int level, std::uint8_t index);   // кламп 0..15
HudLutLevel  level_report(const HudLut&, int level,
                          std::span<const std::uint8_t> palette = {});
std::string  describe(const HudLut&, std::span<const std::uint8_t> palette = {});
bool  build_hud_lut(std::span<const std::uint8_t> palette_rgba, HudLut&,
                    std::string* error = nullptr);                // §2b
bool  hud_lut_skip_slot(int index);                               // §2b
bool  build_model_lut(HudLut&, int max_level, std::string* error = nullptr);

} // namespace gm
```

Ключевые решения:

* **`level` клампится в `0..15`, а не `0..11`.** У движка кламп 15
  (`k = (field + 0x80) >> 4`, `docs/HUD_AUDIT.md` §2.7), и 12..15 — реальные
  таблицы (см. §2), а не алиасы 11. Кламп в 11 остаётся только в модели
  `build_model_lut`/`fade_rgba`, где он осознанный (RGBA-путь не умеет
  осветление, §4).
* **Ровно 4096 байт.** Таблицы позиционные (`levels[k] = bytes[k*256 .. ]`),
  поэтому короткий файл молча сдвинул бы все уровни, а длинный — спрятал бы
  второй payload. Пустой файл, 4095, 4097 и отсутствующий файл — отказ с
  текстом, где есть путь и фактический размер; `lut` при отказе обнуляется.
* `load_hud_lut` заполняет `stats` только при успехе: `source` (путь как
  передан), `bytes` (4096), `levels` (16), `entries` (256).
* `level_report`/`describe` считают «тождество по индексу»
  (`levels[k][i] == i`) и «тождество по цвету»
  (`palette[lut[i]] == palette[i]` по RGB). Второе сильнее: индексы `0` и `1`
  в `GAMEPAL` оба чёрные. Без палитры (`std::span` не 1024 байта)
  `colour_identity == -1`, и `describe` печатает `colour-identity n/a` —
  «не измерено» никогда не выглядит как ноль.
* `describe` — ровно 16 строк, вывод теста = таблица §2:
  `level 12: identity 130/256 colour-identity 132/256 white 1 min 1 max 255`.
* `build_model_lut(lut, max_level)` — модель `(v*k + max/2)/max` при
  `k = min(level, max_level)`, `max_level` в `1..15` (в структуре только 16
  уровней, больше — отказать). При `max_level = 11` это ровно
  `(v*k + 5)/11` — тот же закон, что `fade_lut`/`fade_channel`.

## 2. Что доказано дампом

Файл: `~/.cache/gunmetal-re/hud_lut.bin`, 4096 Б,
FNV-1a64 `0x6330406254771f3d` (закреплён в тесте: другой дамп сломает числа, а
не «тихо позеленеет»). Палитра — `FRONTEND/GAMEPAL` (`gm/palette.py::installed`,
`res/bmp.h::installed_palette`), 256 RGBA-квадов.

Та же таблица получена независимо: Python-скрипт ниже и вывод теста совпадают
до числа.

```sh
python3 - <<'EOF'
import os, sys
sys.path.insert(0, ".")
from gm import palette
game = os.path.expanduser("~/.cache/gunmetal-re/game")
pal = palette.installed(game)
rgb = [tuple(pal[i*4:i*4+3]) for i in range(256)]
dump = open(os.path.expanduser("~/.cache/gunmetal-re/hud_lut.bin"), "rb").read()
print("level idx-id col-id white min max distinct")
for k in range(16):
    t = dump[k*256:(k+1)*256]
    idx = sum(1 for i in range(256) if t[i] == i)
    col = sum(1 for i in range(256) if rgb[t[i]] == rgb[i])
    print(f"{k:5d} {idx:6d} {col:6d} {t.count(255):5d} {min(t):3d} {max(t):3d} "
          f"{len(set(t)):8d}")
EOF
```

| уровень | тождеств по индексу | по цвету | белых | min | max | различных |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 21 | 22 | 1 | 1 | 255 | 21 |
| 1 | 21 | 22 | 1 | 1 | 255 | 32 |
| 2 | 21 | 22 | 1 | 1 | 255 | 45 |
| 3 | 21 | 22 | 1 | 1 | 255 | 57 |
| 4 | 21 | 22 | 1 | 1 | 255 | 69 |
| 5 | 21 | 22 | 1 | 1 | 255 | 85 |
| 6 | 21 | 22 | 1 | 1 | 255 | 101 |
| 7 | 22 | 23 | 1 | 1 | 255 | 120 |
| 8 | 23 | 24 | 1 | 1 | 255 | 138 |
| 9 | 33 | 34 | 1 | 1 | 255 | 156 |
| 10 | 68 | 70 | 1 | 1 | 255 | 183 |
| **11** | **254** | **256** | 1 | 1 | 255 | 254 |
| **12** | **130** | **132** | 1 | 1 | 255 | 210 |
| **13** | **71** | **73** | 1 | 1 | 255 | 184 |
| **14** | **46** | **48** | 1 | 1 | 255 | 159 |
| **15** | **43** | **45** | 1 | 1 | 255 | 160 |

Что из этого следует (данные, не модель):

* **16 таблиц действительно построены**, и они позиционные: тест грузит
  синтетический файл `levels[k][i] = (i + k) & 0xFF` и проверяет все
  16×256 `map`, то есть раскладка «уровень = блок 256 байт», а не сдвиг.
* **Верх кривой — уровень 11.** По цвету 256/256 тождеств, по индексу
  254/256: `0 → 1` (оба чёрные) и `65 → 48` (у обоих RGB `(35,0,0)`). То есть
  «254/256 совпавших записей» из `docs/HUD_AUDIT.md` §2.7 — это ровно разница
  между индексным и цветовым тождеством.
* **Низ кривой — уровень 0.** 236/256 записей уходят в чёрный (почти все — в
  индекс 1), 20 слотов сохраняют свой цвет: 173, 174, 175, 191, 240..255 —
  те же слоты, где `FLAG/naUSA.BMP` расходится с `FRONTEND/GAMEPAL`
  (`docs/UPSCALE.md` §3.2; текст докстринга `gm/palette.py`). Плюс индекс 1
  и так чёрный, поэтому «тождеств по цвету» 22, а не 21.
* **12..15 — не «почти тождественны»: 132/73/48/45** тождеств по цвету,
  число различных записей 210/184/159/160. Формулировка
  `ui/pager_render.h` («12..15 behave like 11 (the tables are near-identity)»)
  этими числами опровергается: фактор `k/11 > 1` осветляет.
* **Инвариант белого.** Ни одна таблица не трогает индекс 255: `map(k,255)
  == 255` при всех `k` (в каждой таблице ровно одна запись, равная 255).
  `min == 1` везде: индекс 0 не пишется никогда — движок пишет только
  индексы из рамп, а «нулевой» результат — это индекс 1.

## 2b. Построение без дампа: `build_hud_lut`

Срез 7: таблицы строятся в C++ из установленной палитры. Формула `0x483290`
разобрана побайтово в `docs/LUT_BUILDER.md` (там же Python-оракул
`tools/re/lut_build.py`); здесь — тот же код в движке.

```cpp
bool build_hud_lut(std::span<const std::uint8_t> palette_rgba, HudLut& out,
                   std::string* error = nullptr);
bool hud_lut_skip_slot(int index);   // 20 слотов 0x483250
```

`palette_rgba` — 256 RGBA-квадов (1024 Б) в порядке `FRONTEND/GAMEPAL`, как их
отдают `res/bmp.h::installed_palette` и `gm/palette.py::installed` (alpha не
используется). Шаги (`docs/LUT_BUILDER.md` §1–§2):

1. слот `i`: `component >> 2` — палитра **6-битная** (`0x47f0b4`; потребитель
   клампит `0x3f`), метрика имеет смысл только в `0..63`;
2. `factor = double(k) * 1/11`; `0x4a2978` — double `0x3FB745D1745D1746`;
3. цель: `trunc(component * factor)` (`0x40e0e0` ставит x87 RC=11) —
   **усечение**, не округление;
4. слоты `{173,174,175,191} ∪ {240..255}` (`0x483250`) не трогаются:
   `dst[i] = i` (`hud_lut_skip_slot` — тот же предикат);
5. иначе — ближайший слот `1..254` по метрике `0x4228e0` со строгим `<`
   (при равенстве остаётся меньший индекс).

**Доказательство.** Палитра из установки (`/mnt/d/games/Gunmetal 3dfx`),
сравнение с дампом — тест `gm_hud_lut_tests`, блок `built`:

```
built: 16x256 from /mnt/d/games/Gunmetal 3dfx (256 RGBA quads), FNV-1a64 0x6330406254771f3d
built: level 11 colour identity 256/256, level 0 236/256 black and 20/256 skip slots untouched
built: level 3 slot 54 (6-bit (44,5,5)) -> 49 (extended precision would say 48)
built vs dump: identical bytes=4096/4096, FNV 0x6330406254771f3d vs 0x6330406254771f3d, first divergent byte: none
built vs dump per level: 0:256 1:256 … 15:256
```

То есть C++-сборка совпадает с дампом **побайтово**: 4096/4096, FNV-1a64
`0x6330406254771f3d` закреплён в тесте. Независимая сверка — тот же результат
у `python3 tools/re/lut_build.py --build` (`docs/LUT_BUILDER.md` §4).

**Отказы.** Пустой/`nullptr` span и любой размер ≠ 1024 Б — `false` + текст
(`... got none`, `palette is N bytes, expected 1024 (256 RGBA quads)`), `out`
обнулён: короткая палитра молча сдвинула бы все слоты.

**Ограничения.** Нужна палитра; порядок слотов = `GAMEPAL` (другая палитра
даст другие таблицы — это свойство кода, а не ошибка). Таблица остаётся
**индекс → индекс**: цвет вызывающий берёт из палитры. Без дампа блок `built`
печатает `SKIP` для сравнения, но закреплённые свойства проверяет всегда:
уровень 11 — 256/256 по цвету, уровень 0 — 236 чёрных + 20 нетронутых,
12..15 — 132/73/48/45 цветовых тождеств.

**Double, а не extended.** `0x4832d1` вызывает `0x40e0e0`, а тот меняет только
RC-биты управляющего слова (`or ah,0xc` перед `fistp`), precision control не
трогает. При PC=53 (`double`) произведение `fild c` / `fmul factor`
округляется до 53 бит, поэтому модель на `double` точна, а не «эмпирически
близка»: вариант «держать произведение в 80-битном extended и усекать из
него» даёт **4060/4096** — расходятся 36 слотов из 3776 (например
`k=3, i=54`: слот `(44,5,5)` в 6 битах, exact `44*3/11` чуть меньше 12 →
extended даёт 11 → слот 48, dump и `double` дают 12 → слот 49; тест
закрепляет `map(3,54) == 49`).

## 3. Модель `(v*k + max/2)/max` против дампа

`build_model_lut(lut, 11)` — та же формула, которой `fade_rgba`/`fade_channel`
красят RGBA-кадр. Сравнение честное только в пространстве цвета: таблица
отображает **индекс в индекс**, поэтому «совпало» = `palette[lut[k][i]]`
равен покомпонентно модели, применённой к `palette[i]`. Для справки рядом —
совпадение в индексном пространстве (`lut[k][i] == model[k][i]`).

| уровень | совпало по индексу | по цвету |
|---:|---:|---:|
| 0 | 0 | 236 |
| 1 | 1 | 4 |
| 2 | 8 | 3 |
| 3 | 6 | 3 |
| 4 | 9 | 8 |
| 5 | 8 | 5 |
| 6 | 10 | 6 |
| 7 | 11 | 4 |
| 8 | 10 | 5 |
| 9 | 9 | 3 |
| 10 | 10 | 3 |
| 11 | 254 | **256** |
| 12 | 130 | 132 |
| 13 | 71 | 73 |
| 14 | 46 | 48 |
| 15 | 43 | 45 |

Выводы:

* **Концы кривой модель воспроизводит.** Уровень 11 — 256/256 по цвету
  (254/256 по индексу: `0 → 1` — тот же чёрный). Уровень 0 — **236/256**, а
  не 256: модель обнулила бы все 256 каналов, а таблица движка оставляет
  20 слотов вне рамп как есть (в том числе белый 255). Это уточнение к
  ожиданию «256 на 0»: точное число — 236.
* **Середина — расхождение, а не паритет.** Уровни 1..10 совпадают по цвету в
  3..8 записях из 256. Модель делает линейное умножение канала, движок —
  переквантовку **индекса** по палитре (пример `docs/UPSCALE.md`
  §3.3: при уровне 10 индекс 2 `(23,23,23)` уходит в 144 `(19,15,15)`).
  Формула `0x483290` разобрана в срезе 7 (`docs/LUT_BUILDER.md`) и повторена
  в C++ — §2b; дамп остаётся проверкой, а не источником.
* **12..15 модель не воспроизводит и не пытается**: она клампит `k` к 11,
  поэтому её цветовое совпадение там равно цветовому тождеству таблицы
  (132/73/48/45). Осветление — свойство таблиц, а не формулы.

## 4. Доказано / модель / гипотеза

**Доказано (код + дамп + тест):**
16 таблиц по 256 байт; позиционная раскладка; уровень 11 — цветовое
тождество 256/256 и индексное 254/256 с двумя известными заменами; уровень 0 —
236/256 чёрных и 20 нетронутых слотов вне рамп; 12..15 = 132/73/48/45
тождеств по цвету (не near-identity); белый слот 255 и `min == 1` во всех
таблицах; отсутствие/неверный размер файла отвергаются текстом.
Срез 7: те же 4096 Б **строятся** из палитры (`build_hud_lut`), совпадение с
дампом побайтовое — §2b; список 20 нетронутых слотов = код `0x483250`
(`docs/LUT_BUILDER.md` §3), а не список рамп.

**Модель (помечена как модель):**
`build_model_lut`/`fade_rgba` — закон `(v*k + max/2)/max` с клампом к 11.
Совпадает с таблицами на концах (11 — побайтово, 0 — 236/256 по цвету) и
расходится в середине; это измерено числами §3, а не заявлено.

**Гипотеза:**
почему исключены именно эти 20 слотов (они `NEONS.PIN`, т.е. вне рамп) —
список проверен кодом, намерение выведено из `docs/LOOK16.md` §1.

## 5. Ограничения и следующий срез

* **Таблица — индекс → индекс.** В RGB-кадр она один-в-один не переносится:
  движок переквантовывает палитровый индекс, а наш кадр — `0xRRGGBBAA`.
  Поэтому `fade_rgba` продолжает работать канальной моделью, а этот модуль
  отдаёт только индексы; для цвета вызывающий сам применяет
  `FRONTEND/GAMEPAL` (`res/bmp.h::installed_palette`, 256 RGBA-квадов).
* **Не подключено.** Модуль — библиотека + тест. `fade_rgba`,
  `ui/pager_render.*`, `main.cpp`, CLI не менялись; палитрового пути
  (индекс → LUT индексов → палитра) в движке пока нет.
* **Нужна палитра.** `build_hud_lut` требует 1024 Б `FRONTEND/GAMEPAL` в
  порядке слотов палитры; без игры/палитры построить таблицы нечем (отказ с
  текстом). Дамп для построения не нужен.
* **Дамп не в поставке.** Без `~/.cache/gunmetal-re/hud_lut.bin` сравнение
  печатает `SKIP`, а закреплённые свойства (§2b) проверяются всегда; файл
  снимается на Windows `tools/win/dump_lut.lua`
  (`docs/HUD_AUDIT.md` §2.8).
* **Следующий срез:** палитровый путь (кадр 8 bpp → `HudLut` → палитра) и
  подключение к `--hud`/полосе, чтобы уровни 12..15 стали воспроизводимыми,
  а не только описанными.

## 6. Проверка (выполненные команды)

```sh
cmake --build build/sandbox --target gm_hud_lut_tests -j 4
ctest --test-dir build/sandbox -R '^hud_lut$'        # 1/1 passed
python3 tools/re/lut_build.py --build                 # identical bytes=4096/4096
python3 regress.py --mirror --json                    # см. отчёт среза
```

Вывод теста (сокращённо, числа дословно) — зелёный прогон, из которого
собраны §2, §2b и §3:

```
synthetic missing: hud_lut: cannot read '/tmp/gm_hud_lut_tests/missing.bin'
synthetic 4095 bytes: hud_lut: '...bad_4095.bin' is 4095 bytes, expected 4096 (16 x 256)
synthetic 4097 bytes: hud_lut: '...bad_4097.bin' is 4097 bytes, expected 4096 (16 x 256)
synthetic empty: hud_lut: '...empty.bin' is 0 bytes, expected 4096 (16 x 256)
synthetic good: source=... bytes=4096 levels=16 entries=256
synthetic map: level 0 -> 0, 11 -> 11, 15 -> 15, 99 -> 15 (index 0)
synthetic skip list: 20 slots, 173..175+191+240..255, 0/1/239 not skipped
synthetic build: no palette refused: hud_lut: build needs the 1024-byte palette, got none
synthetic build: 1020-byte palette refused: ... 1020 bytes, expected 1024 (256 RGBA quads)
synthetic build: 16x256 from a synthetic palette, 320/320 skip entries kept, min entry 1, 0 never produced
built: 16x256 from /mnt/d/games/Gunmetal 3dfx (256 RGBA quads), FNV-1a64 0x6330406254771f3d
built: level 11 colour identity 256/256, level 0 236/256 black and 20/256 skip slots untouched
built: level 3 slot 54 (6-bit (44,5,5)) -> 49 (extended precision would say 48)
built vs dump: identical bytes=4096/4096, FNV 0x6330406254771f3d vs 0x6330406254771f3d, first divergent byte: none
built vs dump per level: 0:256 1:256 ... 15:256
real dump: .../hud_lut.bin, 4096 bytes, FNV-1a64 0x6330406254771f3d
real level 0: 236/256 entries black, 21/256 indices untouched, index 255 -> 255
real model: agreement with the dump is 3..8/256 in the middle (levels 1..10),
            ends 236 and 256/256 by colour
real map: index 3 -> level 0:1 11:3 15:192 99:192, clamp 99==15 on 256/256
hud_lut: ok
```

## 2c. CLI без дампа (`--fade-lut` и `build_hud_lut`, срез 7)

`--pager/--briefing ... --fade-lut N` берут дамп, если он есть
(`$GM_HUD_LUT` или `~/.cache/gunmetal-re/hud_lut.bin`), а **если дампа нет —
строят таблицы из `FRONTEND/GAMEPAL`** вызовом `build_hud_lut()`
(`main.cpp::apply_fade_lut`). Проверено: с убранным дампом кадры уровня 11 и
13 побайтово совпали с кадрами по дампу (md5 `f368a794…` и `12927d97…`),
CTest `cli_fade_lut_built` гоняет тот же compare-скрипт с
`GM_HUD_LUT=<несуществующий путь>`.

Ограничение: встроенная таблица строится из палитры **этой** установки; дамп
остаётся оракулом для сверки (4096/4096), но больше не обязателен.
