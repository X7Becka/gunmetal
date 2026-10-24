# Резолв кадров эффектов (M4.2.6, срез 2)

Что здесь: C++-резолвер «отрицательный индекс текстуры → серия `.sbw` → кадр →
текстура кадра → архив». Источник фактов — `docs/EFFECTS.md` §1–§4 и
`docs/SOA.md` §2–§4; этот документ фиксирует API, числа и границы среза, а не
пересказывает реверс.

Смежное: `engine/src/game/effect_anim.h` (срез 1 — `effect_for_texture`,
`effect_frame_index`), `engine/src/res/sbw.h` (таблицы 1–2 и поля полигона),
`engine/src/res/sbm.h` (payload кадра), `docs/FX_FRAME.md` (этот файл).

---

## 0. Коротко

| вопрос | ответ | статус |
|---|---|---|
| что делает модуль | `-k` → `effects[k-1]` **этого** `.sbw` → кадр на тике → id текстуры → проверка в `PICS`/`sprites SBM.IDX` | доказано (`0x42aa63`, `0x41de7d`, `0x423d36`..`0x423db1`) |
| где живёт | `engine/src/render/fx_frame.{h,cpp}`, namespace `gm::fx` | — |
| подключён к кадровому проходу | **нет**, это read-only резолвер | ограничение |
| порядок рисования | один проход, порядок полигонов; ключа сортировки нет | доказано (`0x41ac99`..`0x41add0`, §4.2) |
| `material`/`lightmap` влияют на ранг | нет; setup их в этой ветке не читает | доказано (§1.4) |
| индекс `0`/положительный | отказ с текстом (это не эффект) | — |
| пустой/битый каталог | `nullopt` + текст, без крэша | — |
| корпус 36 уровней | 6135 эффект-полигонов, 262 пары, 1274 ссылки на кадры, 212 текстур, **0 нерезолвнутых** | тест `fx_frame` |

---

## 1. Что доказано кодом и данными

1. **`-k` = запись таблицы эффектов того же файла.** Загрузчик `0x42aa63`
   читает таблицу 2 подряд (`j` = индекс в файле) и затирает прошлую
   (`docs/EFFECTS.md` §2.1); поиска по имени нет. Следствие: одинаковый `-2`
   — разные серии в разных уровнях (`BRIEF` в `level0`, `WATER2` в `dm1`) —
   проверено тестом.
2. **Запись `.sbw` = рантайм-запись**: `mode +0x00`, `period +0x04` (тики
   91 Гц), `declared +0x08`, `frames +0x0c` (1-based id в реестре текстур
   этого файла), имя `+0x4c`. Совпадение с авторским `.SOA` — 262/262 пары
   (`docs/EFFECTS.md` §2.2). Поэтому `SoaCatalog` хранит именно
   `SbwEffectRef`, а не второй тип с риском разъезда.
3. **Кадр** = `effect_frame_index(mode, period, |ids|, t)`, `t = now −
   poly[+0x68]` (`0x41de7d`, `docs/SOA.md` §4). В поставке `poly[+0x68] = 0`
   у всех полигонов, поэтому вызывающий передаёт тик рендера как `now_tick`.
4. **Кадр — это id текстуры, а не отдельный объект**: `frames[i]` — 1-based id
   реестра `0x76ed40`, запись реестра даёт `kind` (0 `pics\`, 1 `sprites\`,
   4 `flaganim\`) и имя; архив выбирается по `kind` (`0x423d49`/`0x423db1`).
   Для эффект-кадров документированы только `pics`/`sprites`
   (`docs/EFFECTS.md` §2.3), поэтому `kind 4` — отказ, а не догадка.
5. **Один проход.** Drawer полигона вызывается из общего цикла
   (`0x41ac99`..`0x41add0`) и не смотрит на индекс; отдельной очереди,
   прохода или сортировки эффектов нет, глубина — обычная глубина полигона
   (`docs/EFFECTS.md` §4.2). Отсюда `draw_order_rank` — константа.
6. **Гейт `n_idx <= 2`** стоит до чтения индекса (`0x9fde93`), поэтому
   `make_soa_catalog` такие полигоны пропускает. В поставке их 0 на
   эффект-пути (тест сходится с `effects.py`).

### 1.1 Уточнение к прозе §3.1: `handle` и оверлей

Инструкция выбирает источник индекса так (`0x9fde8e`..`0x9fdefc`):

```c
handle = word[poly + 0x72];                       // disk slot
index  = (handle < 1) ? i16 word[poly+0x00]       // texture
                      : i16 word[poly+0x78];      // overlay
```

То есть оверлей становится индексом при `handle >= 1`, текстура — только при
`handle == 0`. Это же правило в `tools/re/effects.py` (`link = poly.handle`,
`return texture if link < 1 else overlay`) и именно оно даёт документированные
**18** «overlay used» и **6135** эффект-полигонов. Прозу §3.1
(«`link_sector != 0`») буквально читать нельзя: если считать
`link_sector = handle − 1`, то `link_sector != 0` даёт 10 полигонов, а не 18 —
расхождение на 8 полигонах с `handle == 1`. Измерено: `overlay < 0` всего
133, из них `handle >= 1` — 18 (`handle > 1` — 10, `handle == 1` — 8).
C++ следует инструкции; тест закрепляет 18/6135.

---

## 2. API (`engine/src/render/fx_frame.h`)

```cpp
namespace gm::fx {

struct FxTextureRef { std::string name; int kind = 0; };

struct SoaCatalog {                    // вид уровня, собран из одного .sbw
    std::string level;
    std::vector<SbwEffectRef> effects;   // таблица 2: -k -> effects[k-1]
    std::vector<FxTextureRef> textures;  // таблица 1: id кадра 1-based
    std::vector<int> negative_indices;   // по одной записи на эффект-полигон
};

struct SbmCatalog {                    // имена из PICS/ и sprites/ SBM.IDX
    std::vector<std::string> pics;
    std::vector<std::string> sprites;
    bool contains(int kind, const std::string& name) const;  // без регистра
    const char* archive_name(int kind) const;                // "PICS"/"sprites"
    bool empty() const;
};

SoaCatalog make_soa_catalog(const SbwLevel& world, std::string level = {});

struct FxFrameKey { int poly_texture; std::uint16_t material; std::uint16_t lightmap; };
struct FxFrameRef { int series_index; std::string series_name;
                    int frame; int frame_count; bool animated; };

std::optional<FxFrameRef> resolve_fx_frame(int negative_index,
                                           const SoaCatalog&, const SbmCatalog&,
                                           std::string* error = nullptr,
                                           std::int64_t now_tick = 0);

std::vector<FxFrameRef> frames_for_level(const SoaCatalog&, const SbmCatalog&,
                                         std::int64_t now_tick = 0,
                                         std::vector<std::string>* errors = nullptr);

constexpr int kFxSinglePass = 0;
int draw_order_rank(FxFrameKey key);
}
```

Почему `gm::fx`, а не `res/soa.h`: в `master` (база `5d3bdab`) **нет**
`engine/src/res/soa.h` и типов `SoaCatalog`/`SbmCatalog` — C++-читателя `.soa`
не существует, есть только `res/anim.h::parse_soa`. Срез не заводит второй
парсер `.soa`: каталог строится из уже проверенного `res/sbw.h`, а типы
помещены в `gm::fx`, чтобы будущий `res/soa.h` не столкнулся с ними по имени.
Переход на будущий `SoaCatalog` — замена одного аргумента, поля записи те же.

### 2.1 Контракт `resolve_fx_frame`

* `*error` очищается при успехе и заполняется самодостаточной фразой при
  любом отказе; `nullptr` допустим.
* `series_index = -k − 1`, `series_name` из записи, `frame` — 0-based индекс
  **внутри серии** на тике `now_tick`, `frame_count = |ids|`.
* `animated = (mode == 1 && frame_count > 1)`: двигается именно индекс кадра.
  Режимы 100/101/102 перерисовывают **одну** текстуру (часы/титр,
  `docs/SOA.md` §6) — `animated` у них `false`, live-перерисовка не
  моделируется.
* Отказы (каждый с текстом): `index >= 0`; пустая таблица эффектов; пустой
  `SbmCatalog`; `-k` за таблицей; запись без `ids`; нет кадра на этом тике
  (в т.ч. `mode 1` с `period == 0` и отрицательный тик — у `0x41de7d` клампа
  нет); `frame id == 0`; id за реестром текстур; `kind ∉ {0,1}`; пустое имя
  текстуры; имени нет в архиве своего `kind`.
* Имена архивов сравниваются без регистра: `SBM.IDX` хранит `BRIEF1A`,
  `.sbw` — `Brief1a`; оригинал ищет по BIGFILE-индексу в Windows-семантике.

### 2.2 `frames_for_level` и `draw_order_rank`

* `frames_for_level` берёт **уникальные** отрицательные индексы уровня,
  сортирует по `series_index` (`-1` первой), резолвит на одном тике; порядок
  детерминирован, повторный вызов даёт тот же набор. Нерезолвнутые индексы
  не попадают в результат, причины — в `*errors` (функция тотальна на битом
  мире).
* `draw_order_rank` всегда `kFxSinglePass = 0`. Это не заглушка «на потом», а
  доказанный факт §4.2: эффект рисуется тем же drawer'ом в том же порядке, а
  `material`/`lightmap` в этой ветке не читаются. Параметр-ключ оставлен,
  чтобы второй проход нельзя было добавить молча.

---

## 3. Числа теста (`ctest -R fx_frame`)

```
corpus: 36 levels, texture<0=6117 overlay<0=133 overlay-selected=18
corpus: effect-polygons=6135 pairs=262 series=49/48 frame-refs=1274 textures=212 unresolved=0
frames_for_level level0 : 7 series -1=FILM/1 -2=BRIEF/3 -3=COMP2/2 -4=COMP1/2 -5=DCLOCKLG/1 -6=COMPAN2/2 -7=TELEP1/6
frames_for_level dm1    : 7 series -1=WATER1/6 -2=WATER2/4 -3=COMP2/2 -4=FILM/1 -5=COMP1/2 -6=FLAMES/8 -7=TELEP1/6
fx_frame: all checks passed
```

* Совпадает с `docs/EFFECTS.md` §3: 6117 `texture<0`, 133 `overlay<0`,
  18 «оверлей как индекс», 6135 эффект-полигонов; 262 пары (уровень, эффект),
  1274 ссылки на кадры, 212 уникальных текстур, 0 нерезолвнутых (§3.3).
* `series=49/48`: 49 разных написаний в `.sbw`, 48 без регистра. Док говорит
  «48 имён» — это `WATLAVA` (большинство уровней) против `watlava`
  (`level6`). Тест закрепляет оба числа, чтобы разница не потерялась.
* `level0`: 68 полигонов с отрицательным индексом (§3.2), 7 серий; кадры
  `BRIEF` (3 кадра / 65 тиков) на тиках 0/22/44/65 — 0/1/2/0; `TELEP1`
  (6 кадров / 21 тик) — `frame = tick*6/21` на всех 21 тиках.
* `dm1`: 134 полигона, 7 серий; 9 «сырых» `overlay<0`, но ни один не служит
  индексом (`handle == 0`) — согласуется с `effects.py` (`overlay_used = 0`).
* Синтетика: все девять отказов, пустые каталоги (включая `error = nullptr`),
  `id 0`, пустое имя, отрицательный тик, `kind 4`, отсутствие в архиве,
  дедуп/порядок/детерминизм `frames_for_level`, 36 комбинаций
  `draw_order_rank`.

Команды:

```sh
cd /home/nooverload/build/gm-sub-fxframe
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox --target gm_fx_frame_tests -j 4
./build/sandbox/engine/tests/gm_fx_frame_tests
python3 regress.py --mirror --json
```

---

## 4. Ограничения среза

1. **Не подключён к кадровому проходу.** Нет вызова из `render.cpp`/`main.cpp`
   и нет CLI-строки: модуль только отвечает на вопрос «что рисовать», но
   кадровый проход по-прежнему видит отрицательный индекс как «нет текстуры».
2. **Нет времени.** `resolve_fx_frame` принимает `now_tick` снаружи; часов
   `[0x9e82d8]`, вычитания `poly[+0x68]` и прогона кадров во времени в модуле
   нет. Фаза на реальном кадре не проверялась (в оригинале — наблюдением).
3. **Нет анимации во времени, блендинга и приоритетов.** `animated` — признак
   «индекс кадра двигается», а не плеер. Alpha/blend эффектов не доказаны
   (`docs/EFFECTS.md` §4.3): материал полигона и маска кадра не
   интерпретируются. Порядок — константа (§4.2), сортировки/глубины нет.
4. **Спецрежимы 100/101/102** дают `frame 0` и `animated = false`;
   дорисовка текста (`0x44dc50`) и live-перерисовка текстуры не
   воспроизведены.
5. **`.soa` не читается.** `SoaCatalog` — это `.sbw`-таблица (рантайм-вид);
   авторские `anims/*.SOA` совпадают с ней 262/262, но парсер `.soa`
   (`res/anim.h`) в этом срезе не используется. Отдельный `.soa`-каталог —
   следующий срез, если понадобится фаза/имена из авторского файла.
6. **SBM-архив только по именам.** `SbmCatalog` заполняется списком
   `BigFile`-записей (`PICS/SBM.IDX`, `sprites/SBM.IDX`); сами кадры при
   отрисовке читает существующий `res/sbm.h` (`parse_sbm`), здесь payload не
   разбирается и размеры кадров не проверяются. Достижимые кадры поставки все
   `kind 0`; `sprites`-ветка закреплена только синтетикой (объявленные, но
   недостижимые полигонами `mode 0` дают 6520 ссылок в `sprites`,
   `docs/EFFECTS.md` §2.3).
7. **Спавны/объекты.** Резолвер покрывает только полигоны уровня; путь
   спрайт-объектов (`obj+0x20` → `0x41de7d`, `docs/SOA.md` §4) использует тот
   же селектор, но `SPRITES` в поставочных `.sbt` нет (0 записей), поэтому
   здесь не проверялся.
