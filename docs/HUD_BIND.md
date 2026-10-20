# HUD binding: `.sml`/`.sme` экраны → семейства `.sof`/`.fdx` и позиции `<I>`

* **Статус:** типизированная связка «экран SML → семейство HUD-фильма → имя/строка
  индекса → позиция элемента». Разметка читается существующим `res/sml.*`,
  фильм грузится существующим `render/hud_player.*`; новых парсеров нет.
* **Ветка:** `feat/hud-bind`, база `master` `377b867`, дата 2026-10-20.
* **Игра:** только чтение (`SML/**`, `Film/*.sof`, `Film/*.fdx`), ничего не
  пишется; синтетический фикстур для тестов лежит в системном tmp.
* **Файлы:** `engine/src/render/hud_bind.h`, `engine/src/render/hud_bind.cpp`,
  `engine/tests/test_hud_bind.cpp`; строка `src/render/hud_bind.cpp` в
  `engine/CMakeLists.txt`, цель `gm_hud_bind_tests`/тест `hud_bind` в
  `engine/tests/CMakeLists.txt`.
* **Эталон:** `docs/UI.md` (язык, `<I>`, позиции, экраны), `docs/HUD_PLAYER.md`
  (`load_hud_film`, `WEAPON8` → строка 8), `docs/FILM.md` §1.3/§2/§3
  (`0x4958d0`, префиксы, `.fdx`), `engine/src/res/sml.*`.

`render_scene`, `hud.*`, `hud_player.*`, `res/sml.*` не менялись: модуль только
читает `SmlDocument` и вызывает `load_hud_film`.

## 1. Модель

```cpp
enum class HudFamily { Default, Weapon, Ammo, Tank, Shield, Equip };
enum class HudBasis  { Proven, Hypothesis };   // доказано xref / гипотеза
enum class HudAnchor { None, Left, Right };    // <I a=l|r>

struct HudScreenClass {
    std::string stem;      // basename без расширения, в верхнем регистре
    HudFamily family;      // WEAPON/AMMO/TANK/SHIELD/EQUIP/DEFAULT
    bool has_id;           // в имени есть числовой суффикс
    int id;                // он же номер строки .fdx
    HudBasis basis;        // Proven для <FAMILY><id>, иначе Hypothesis
};

struct HudElementBinding {  // один <I>
    std::size_t index, line;
    std::string image;      // f= (loose BMP, как в разметке)
    int x, y;               // атрибуты
    HudAnchor anchor;
    HudBasis position_basis;
    bool x_reserved;        // anchor задан, x не двигает слот
};

struct HudBinding {
    std::string screen;          // аргумент parse_binding
    HudScreenClass screen_class;
    std::vector<HudElementBinding> images;
    HudFamily family() const; bool has_id() const; int id() const;
};
```

`HudBinding` — это «экран + его слоты»: семейство и id берутся из имени экрана,
слоты — из `SmlDocument::elements` (`SmlKind::Image`), в порядке документа.
Каждый слот помнит, доказана его позиция или нет, и не теряет `x`, даже когда
его перекрывает anchor.

## 2. API

```cpp
HudScreenClass classify_screen(std::string_view screen);

std::optional<HudBinding> parse_binding(const SmlDocument& doc,
                                        std::string_view screen,
                                        std::string& error);

struct HudState {                 // состояние игрока
    HudFamily focus = HudFamily::Weapon;
    int weapon=-1, ammo=-1, tank=-1, equip=-1, shield=-1;
    int slot(HudFamily) const;    // -1 = неизвестно, Default -> 0
};

struct HudFilmRef {
    HudFamily family; HudBasis basis; int id;
    std::string name;             // "WEAPON8", "Default"
    std::string note;             // почему выбрано
    HudFilm film;                 // загруженный .sof + .fdx
};

std::optional<HudFilmRef> resolve_film(const HudBinding&, const HudState&,
                                       const std::filesystem::path& game,
                                       std::string& error);

struct HudPlacement {             // готовый прямоугольник 128×64
    std::size_t index; std::string image;
    HudAnchor anchor; HudBasis basis;
    int x, y, width, height;
    bool clipped, visible;
    int clip_width, clip_height;
};

std::optional<std::vector<HudPlacement>> layout(const HudBinding&, const HudFilm&,
                                                const RenderResult& target,
                                                std::string& error);
std::optional<std::vector<HudPlacement>> layout(const SmlDocument&, const HudFilm&,
                                                const RenderResult& target,
                                                std::string& error);
```

### 2.1. `parse_binding`

`screen` — путь, basename или голый ствол; регистр не важен, расширение
отбрасывается (`.sml`/`.sme`). `classify_screen` понимает `WEAPON<n>`,
`AMMO<n>`, `TANK<n>`, `SHIELD<n>`, `EQUIP<n>`, `DEFAULT`, `TEMP<n>` и всё
остальное относит к `Default`.

Отказы (все с текстом и номером строки там, где он есть):

* пустой документ, нет тегов, нет `<d>`/`</d>`, разбалансированы `<`/`>`,
  превышен лимит 511 тегов (те же проверки, что у `parse_sml`, повторены для
  собранного вручную `SmlDocument`);
* `<I>` без `f`, без `y`, без `x` при отсутствии anchor, нецелые `x`/`y`,
  `a` не `l`/`r`;
* координата вне `±4096` (kHudBindMaxCoord);
* больше 32 слотов на экран (kHudBindMaxImages; в поставке максимум 3).

Ничего не пропускается молча: неизвестный атрибут — ошибка, а не «0 по
умолчанию».

### 2.2. `resolve_film`

Порядок (первое совпадение):

1. в имени экрана есть id → это семейство/id, `basis = Proven`;
2. экран называет семейство, а `HudState` знает его слот → семейство из имени,
   id из состояния, `Hypothesis`;
3. `HudState::focus` знает слот → это семейство/id, `Hypothesis`;
4. иначе `Default.sof` (строка 0), `Hypothesis`, в `note` написано, почему.

Имя собирается как `<PREFIX><id>` (`WEAPON8`, `AMMO4`, `TANK2`, `SHIELD0`,
`EQUIP3`), для `Default` — каноническое `Default` без цифр. Дальше вызывается
`load_hud_film`, поэтому работают все его проверки: нет `.sof`, нет `.fdx`/`.idx`,
строка вне индекса, `count > tiles`, `fps = 0`. Ошибка переделывается в
`hud_bind: <имя>: <причина>`, чтобы было видно, какой фильм не сошёлся.

`EQUIP<n>` — ожидаемый отказ: `equip.fdx` на 6 строк в поставке есть, а
`EQUIP*.SOF` нет вовсе (`docs/FILM.md` §2).

### 2.3. `layout`

Для каждого слота выдаётся прямоугольник фильма 128×64:

| вход | `x` | `y` | basis |
|---|---|---|---|
| нет ни одного `<I>` (полноэкранный дизайн/брифинг) | `14` | `125` | `Proven` |
| `<I a=l>` | `0` | атрибут `y` | `Hypothesis` |
| `<I a=r>` | `target.width − 128` | атрибут `y` | `Hypothesis` |
| `<I>` без `a` | атрибут `x` | атрибут `y` | `Proven` |

Заэкранные прямоугольники не выбрасываются: `clipped` — часть вышла за кадр,
`visible` — пересечение непусто, `clip_width`/`clip_height` — видимая часть.
Полностью невидимый слот остаётся в списке, чтобы вызывающий мог его
зарепортить. Ошибки: target вне `1..8192`, `rgb` не `width*height*3`, фильм с
`count = 0`, `count > tiles` или `fps` вне `1..1000` (те же проверки, что у
`frame_at`).

## 3. Таблица привязок

### 3.1. Экраны-семейства (`SML/DESIGN/`)

Префикс и вызов `film_set` доказаны xref; совпадение «имя страницы = имя
фильма» — данные поставки (`Hypothesis` для конкретной страницы, см. §4).

| экран | семейство | фильм | строка `.fdx` | `.sof` в поставке | позиция без `<I>` |
|---|---|---|---|---:|---|
| `DESIGN/WEAPON<0..20>` (20 экранов) | `WEAPON` | `WEAPON<id>.sof` | `weapon.fdx:<id>` (24 строки) | 1–8, 10–19; нет 0, 9, 20 | (14,125) |
| `DESIGN/AMMO<0..11>` (12) | `AMMO` | `AMMO<id>.sof` | `ammo.fdx:<id>` (16) | 0–7, 9–11; нет 8 | (14,125) |
| `DESIGN/TANK<0..3>` (4) | `TANK` | `TANK<id>.sof` | `tank.fdx:<id>` (8) | все 0–3 | (14,125) |
| `DESIGN/SHIELD0` (1) | `SHIELD` | `Shield0.SOF` | `shield.fdx:0` (1) | да | (14,125) |
| `DESIGN/EQUIP<0..8>` (9) | `EQUIP` | `EQUIP<id>.sof` | `equip.fdx:<id>` (6) | **нет ни одного** | (14,125) |
| `DESIGN/TEMP0` (1) | `Default` (`Hypothesis`) | `Default.sof` | `default.fdx:0` | да | (14,125) |

У всех 47 дизайн-экранов нет `<I>`: это текстовые страницы, фильм — оверлей
`0x4958d0`. Для `WEAPON0/9/20`, `AMMO8`, любого `EQUIP*` `resolve_film`
возвращает `nullopt` с внятной ошибкой — это и есть проверка «не угадывать
молча».

### 3.2. Экраны с `<I>` (`SML/BRIEF/`, 24 экрана, 72 картинки)

Это не семейства фильмов: `classify_screen` даёт `Default`, `resolve_film` —
`Default.sof` (строка 0), а `layout` ставит фильм в каждый слот `<I>`.

| экран | `<I>` | позиции на 320×200 |
|---|---:|---|
| `brief/level1` | 2 | `ltmills` l → (0,64); `lv1swtch` r → (192,64) |
| `brief/level3`, `level4`, `level5`, `level6`, `level7`, `level9`, `level10` | 2 | l → (0,64), r → (192,64) |
| `brief/level12` | 3 | l → (0,64), r → (192,64), l → (0,64) |
| `brief/level11`, `13`, `14`, `15`, `16`, `17`, `18`, `19`, `2`, `20`, `21`, `22`, `23`, `24` | 1 | (0,64) или (192,64) по `a` |
| `help.sme`, `credits`, `prelevel/**`, остальные `brief/**` | 0 | оверлей (14,125) |

Итого по корпусу: **72** `<I>` на **24** уникальных экранах (48 файлов с учётом
`.sml`/`.sme`-пар), **15** уникальных имён BMP, `a=l` — 50, `a=r` — 22, у всех
`x=64 y=64`. Это ровно цифры `docs/UI.md` §5.2/§7.2 и `ui_semantics.py --check`.

## 4. Что доказано, а что гипотеза

### 4.1. Доказано (xref)

| Адрес | Факт | Где используется |
|---|---|---|
| `0x4958d0` | `film_set(prefix,id,...)`: один и тот же `id` идёт в `sprintf("%s%ld")` и в парсер строки `.fdx` → `WEAPON8.SOF` = строка 8 | `resolve_film`: `id` = строка |
| `0x4958d0`/`0x495a08` | `dest = x + 125*pitch + 14`, оверлей 128×64 в строке 125 | `kHudBindFilmX/Y`, `layout` без `<I>` |
| `0x4be894`, `0x4be724`, `0x4be7e8`, `0x4be7e0`, `0x4be778`, `0x4bb344` | литералы префиксов `WEAPON/AMMO/TANK/SHIELD/EQUIP/DEFAULT` | `hud_family_prefix` |
| `0x496d82`, `0x4be7bc` | гараж вызывает `WEAPON` и грузит `SML/DESIGN/TEMP0.SML` — механизм «страница → фильм» | `TEMP0` → `Default` |
| `0x498012` | `<I x y f a>`: `x`/`y` — координаты, `a=l/r` — край, `f` — имя BMP | `parse_binding`, anchor |
| `0x497310`, `0x497650`, `0x497d30` | загрузчик SML, лимит 511 тегов, девять обработчиков | проверки `parse_binding` |

### 4.2. Гипотезы

1. **Страница `WEAPON15` = вызов `film_set("WEAPON",15)`.** Имена дизайн-
   экранов и фильмов совпадают 1:1, механизм вызова доказан, но per-screen
   xref не восстановлен. Поэтому выбор по состоянию (`state.focus`/слоты)
   всегда `Hypothesis`, а не `Proven`.
2. **`0x81c4a0` — слот оружия или тип патронов.** Один глобал на `WEAPON` и
   `AMMO`; `docs/FILM.md` §9.1 оставляет вопрос открытым. `HudState` разделяет
   `weapon`/`ammo`, вызывающий сам решает, что туда положить.
3. **`<I>`-слот как якорь фильма.** В поставке `<I>` ссылается на loose BMP,
   никогда на `.sof`. Использовать слот как позицию фильма — контракт этого
   модуля, а не поведение движка.
4. **Край `a` считается от краёв экрана** (0 / `width−128`). Обработчик
   прижимает картинку к краю текущей текстовой колонки (`0x68e654`/`0x68e64c`),
   origin пагера не добит (`docs/UI.md` §10.5). При `a` атрибут `x` сохраняется
   в `x_reserved`, но позицию не двигает.
5. **`TEMP0` → `Default.sof`.** TEMP-префикс (`0x4be7f0`) — шаблон гаража; своего
   фильма у него нет. Соответствие `Default` — вывод, не xref.

## 5. Golden и проверки

`gm_hud_bind_tests` (CTest `hud_bind`, label `quick`) проверяет:

* корпус: 300 файлов, 299 связаны, 1 известная аномалия (`prelevel/dm4.sml`
  без `</d>`); 72 `<I>`, 15 уникальных имён, 24 экрана, `l/r` = 50/22,
  все `x=y=64`, все позиции с anchor помечены `Hypothesis`;
* дизайн: 47 уникальных экранов — 20 `WEAPON`, 12 `AMMO`, 4 `TANK`,
  1 `SHIELD`, 9 `EQUIP`, 1 `TEMP0`; ни один не уходит в `Default` по ошибке;
* реальные файлы: `help.sme` (0 картинок → оверлей 14,125), `brief/level1.sml`
  (2 слота → (0,64)/(192,64));
* синтетический документ с тремя `<I>` и `DESIGN/WEAPON4` → семейство/id,
  позиции (10,20)/(0,64)/(192,64), `layout(doc)` = `layout(binding)`;
* выбор фильма на временном `Film/`: имя (`WEAPON4` → строка 4, `count=2,
  fps=15` из `.fdx`), состояние (`focus=AMMO`, `ammo=3` → `AMMO3`), семейство
  без id (`WEAPON` + `weapon=4`), `TANK2`, fallback `Default` (строка 0);
* отказы: `WEAPON20` (нет `.sof`), `WEAPON30` (строка вне `weapon.fdx`),
  `EQUIP0` (нет `EQUIP*.SOF`), битые документы (пустой, без тегов, без
  `<d>`/`</d>`, скобки, лимит тегов), `<I>` без `f`/`y`/`x`, нецелые `x`/`y`,
  `a=q`, координата 99999, 33 слота; target 0×0, короткий буфер, `count=0`,
  `count>tiles`;
* clip: оверлей на 320×130 (виден 128×5), на 320×60 (невидим), правый anchor
  на 100 px (`x=−28`, видно 100 колонок).

Прогон:

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx -DGM_BUILD_TESTS=ON
cmake --build build/sandbox --target gm_hud_bind_tests -j 32
ctest --test-dir build/sandbox -R hud_bind --output-on-failure
# 1/1 Test #16: hud_bind ... Passed  2.94 sec
python3 regress.py
# regress: 113/113 checks passed
```

## 6. Что осталось

1. **Per-screen xref.** Найти вызов `film_set` для каждой дизайн-страницы
   (или трассировать `0x4bc440`/`0x68ccf4`), чтобы перевести §4.2.1 из
   гипотезы в доказанное.
2. **Смысл `0x81c4a0`.** Трассировка записи поля в `0x47b423`/`0x495485`
   должна решить, слот это или тип патронов; тогда `HudState` можно заполнять
   из сохранения/движка автоматически.
3. **Origin пагера.** Extents `0x4972f0(0x10b,0xa7)` и колонка `0x68e658`
   дадут точный левый край, после чего anchor можно считать от колонки, а не
   от края экрана.
4. **Композиция.** `layout` + `frame_at` + `hud_player::draw` ещё не собраны в
   один вызов; это следующий слой (позиция уже готова, нужен выбор кадра и
   палитра).
5. **`EQUIP*.SOF`.** Файлов нет в поставке; если появится мод/другая версия,
   `resolve_film` заработает без изменений (`.fdx` уже есть).
6. **Runtime-перезапись `f`** (`0x4980db`) в 2-значный handle не проверена на
   всех 72 картинках; на диске имя остаётся именем, `HudElementBinding::image`
   хранит именно его.
