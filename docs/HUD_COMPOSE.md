# HUD composition: `.sml` + `.sof/.fdx` + `blit_indexed` за один вызов

* **Статус:** одна функция композиции HUD-экрана: путь/байты `.sml`/`.sme` →
  разметка → привязка к семейству фильма → выбор фильма → кадр по тику 91 Гц →
  блит всех placements → сводка. Новых парсеров и форматов нет: модуль только
  связывает `res/sml.*`, `render/hud_bind.*`, `render/hud_player.*` и
  `render/hud.*`.
* **Ветка:** `feat/hud-compose`, база `master` `9c9fd58`, дата 2026-10-20.
* **Игра:** только чтение (`SML/**`, `Film/*.sof`, `Film/*.fdx`,
  `Film/Kflm0000.bmp`); ничего не пишется. Синтетические фикстуры теста лежат в
  системном tmp.
* **Файлы:** `engine/src/render/hud_compose.h`,
  `engine/src/render/hud_compose.cpp`,
  `engine/tests/test_hud_compose.cpp`; строка
  `src/render/hud_compose.cpp` в `engine/CMakeLists.txt`, цель
  `gm_hud_compose_tests` / тест `hud_compose` в `engine/tests/CMakeLists.txt`.
* **Эталон:** `docs/HUD_BIND.md` (привязка экрана, `layout`),
  `docs/HUD_PLAYER.md` (тайминг, golden AMMO4), `docs/HUD_CPP.md`
  (`blit_indexed`), `docs/UI.md` (`<I>`, позиции, `<d>`), `docs/FILM.md`
  (`0x4958d0`, `(14,125)`, `.fdx`).

`render_scene`, `hud.*`, `hud_player.*`, `hud_bind.*`, `main.cpp` не менялись.

## 1. Конвейер

```
path/bytes ──parse_sml──> SmlDocument
               │
               ├─parse_binding(name)──> HudBinding (family, id, <I>-слоты)
               │        └─resolve_film(state, game)──> HudFilmRef + HudFilm
               │                 └─frame_at(tick)──> HudFrame (tile index)
               │                          └─layout(binding, film, target)──> HudPlacement[]
               └──────────────────────────── blit_indexed (по одному на видимый слот)
```

| шаг | вызов | что даёт | отказ |
|---|---|---|---|
| 0 | проверки входа | target 128x64..8192², `rgb == w*h*3`, палитра ≥ 1024 Б | `target ... smaller than`, `palette has ...` |
| 1 | `parse_sml` | декодированный `<d>...</d>` (в т.ч. `.sme`) | пустой/битый документ |
| 2 | `parse_binding` | семейство/id из имени + `<I x y a>` | `<I>` без `f/y/x`, `a` не `l/r`, > 32 слотов |
| 3 | `resolve_film` | имя (`AMMO4`), строка `.fdx`, загруженные `.sof` | нет `.sof`/`.fdx`, строка вне индекса, `count`/`fps` |
| 4 | `frame_at(tick)` | индекс тайла на тике | переполнение `tick * fps` |
| 5 | `layout` | 128x64 прямоугольники + `clipped/visible` | невалидный target/фильм |
| 6 | `blit_indexed` | RGB в target | (после шагов 0–5 не ожидается) |

Порядок важен: **все** проверки, включая выбор кадра, идут до первого блита.
Поэтому любая ошибка оставляет `target` побайтово нетронутым — тест это
проверяет для каждого негативного случая.

## 2. API

```cpp
struct HudComposeSummary {
    std::string screen;        // аргумент вызывающего (путь или логическое имя)
    std::string stem;          // "AMMO4" (classify_screen)
    HudFamily family;          // Ammo/Weapon/Tank/Shield/Equip/Default
    std::string family_name;   // "ammo"
    bool has_id; int id;       // id = строка .fdx
    HudBasis screen_basis;     // имя экрана -> семейство
    HudBasis film_basis;       // выбор семейства/id
    std::string film;          // "AMMO4", "Default"
    std::string note;          // почему этот фильм (из resolve_film)
    int line;                  // физическая строка .fdx
    std::uint32_t count, fps;  // запись индекса
    std::uint64_t tick;        // использованный тик
    std::size_t frame;         // нарисованный тайл
    bool ended;                // non-loop фильм доигран
    std::size_t placements;    // всего прямоугольников
    std::size_t visible;       // непустое пересечение с target
    std::size_t clipped;       // часть или всё вне target
    std::size_t drawn;         // сделано блитов (== visible)
};

std::optional<HudComposeSummary> compose_hud(
    RenderResult& target, const std::filesystem::path& game,
    const std::filesystem::path& screen_path, const HudState& state,
    const std::vector<std::uint8_t>& palette, std::uint64_t tick,
    std::string& error);

std::optional<HudComposeSummary> compose_hud(
    RenderResult& target, const std::filesystem::path& game,
    std::span<const std::uint8_t> screen_bytes, std::string_view screen_name,
    const HudState& state, const std::vector<std::uint8_t>& palette,
    std::uint64_t tick, std::string& error);
```

* `screen_path` — путь, ствол или имя с расширением. Сначала пробуется файл
  как есть, затем тот же ствол с `.sml`/`.sme` (оба регистра). Поиск
  регистронезависимый: относительный путь разрешается через
  `resolve_ci(game, ...)`, абсолютный — через `resolve_ci(parent, filename)`.
  Поэтому `DESIGN/AMMO4.sml` находит поставленный `design/ammo4.sml` и
  `design/ammo4.sme` на Linux.
* `screen_bytes` — уже прочитанный файл; `screen_name` нужен, потому что
  семейство определяется **именем**, а не разметкой.
* `state` (`HudState`) используется только когда в имени нет id: семейство
  экрана → слот из состояния → `focus` → `Default.sof`
  (`docs/HUD_BIND.md` §2.2).
* `palette` — 256 RGBA-квадов (`BmpImage::palette`, `Film/Kflm0000.bmp`).
* `tick` — неотрицательный счётчик 91 Гц; кадр
  `(tick * fps / 91 + start_frame) % count` для `loop` (HUD-политика
  `start_frame = 1`).
* Возврат — `nullopt` + текст в `error`; при успехе `error` очищается.
  Вложенные ошибки сохраняют происхождение: `hud_compose: hud_bind: ...`,
  `hud_compose: hud: tick ... overflows ...`.

### 2.1. Политика блита

Композиция повторяет блит `docs/HUD_PLAYER.md` §4: `scale = 1`,
`opacity = 1`, **без** ключа прозрачности. Индекс 0 фильма — обычный цвет,
поэтому экран `AMMO4` в позиции оверлея даёт побайтово тот же кадр, что
`hud_player` (это и есть golden). Прозрачный режим — опция фронтенда, в этот
API сознательно не входит (§8).

### 2.2. Сводка

`placements` — все прямоугольники `layout`, включая полностью невидимые;
`clipped` — те, у которых часть **или всё** вне кадра; `visible` — непустое
пересечение; `drawn == visible`. Полностью невидимый слот не ошибка: он
остаётся в отчёте, чтобы интерфейс мог его показать/залогировать.

## 3. Golden FNV (поставка)

Сцена: чёрный RGB 320x200, палитра `Film/Kflm0000.bmp`,
`SML/DESIGN/ammo4.sml` (в разметке нет `<I>`, значит фильм — оверлей
`0x4958d0` в `(14,125)`), `HudState{}` (id берётся из имени экрана),
`AMMO4.SOF` = `ammo.fdx:4`, `count = 131`, `fps = 24`, `start_frame = 1`,
`loop`. FNV-1a64 (offset `0xcbf29ce484222325`, prime `0x100000001b3`) по
`320*200*3`:

| tick | кадр | FNV-1a64 | совпадение с `hud_player` |
|---|---|---|---|
| 0 | 1 | `aaf521e923f31099` | побайтово |
| 91 | 25 | `eeb1a6677e96f650` | побайтово |
| 182 | 49 | `505b37daeecc1c76` | побайтово |

Числа — те же, что в `docs/HUD_PLAYER.md` §4 (получены независимым
Python-эталоном `gm/sof.py` + `gm/bmp.py`). Тест дополнительно сравнивает
**весь буфер** с `hud_player::draw(..., 14, 125, transparent=false, ...)`, а не
только хеш; строка 124 остаётся нетронутой как следствие `y = 125`.

## 4. Ошибки

Все пути отказа явные; «молчаливого no-op» нет (пустой `placements` возможен
только если `layout` вернул слоты, а те невидимы — это видно в сводке).

| ситуация | пример сообщения |
|---|---|
| нет `.sml`/`.sme` | `hud_compose: no .sml/.sme screen at 'SML/DESIGN/nope.sml'` |
| пустые байты | `hud_compose: screen 'x.sml' is empty` |
| битая разметка | `hud_compose: sml: ...` (текст `parse_sml`) |
| плохой `<I>` | `hud_compose: hud_bind: <I> line 3 has no y= coordinate` |
| нет фильма (`EQUIP`) | `hud_compose: hud_bind: EQUIP0: hud: film not found: /.../Film/EQUIP0.sof` |
| нет `.fdx` | `hud_compose: hud_bind: AMMO4: hud: no .fdx index for AMMO4.SOF (expected ...)` |
| target меньше тайла | `hud_compose: target 64x64 is smaller than 128x64` |
| битый буфер | `hud_compose: target buffer is N bytes, expected M` |
| нет палитры | `hud_compose: palette has 1020 bytes, need 1024 (256 RGBA entries)` |
| переполнение тика | `hud_compose: hud: tick 18446744073709551615 overflows at fps 15` |

`EQUIP<n>` — ожидаемый отказ поставки: `equip.fdx` на 6 строк есть, а
`EQUIP*.SOF` нет ни одного (`docs/FILM.md` §2). Тест проверяет и синтетический
`equip.fdx` без `.sof`, и настоящий `DESIGN/equip0.sml`.

## 5. Доказано / гипотеза

**Доказано кодом и данными:**

* `film_set` (`0x4958d0`) кладёт один и тот же id и в имя `%s%ld`, и в строку
  `.fdx`; HUD-загрузчик ставит `start_frame = 1`, `loop = 1`; оверлей —
  `x + 125*pitch + 14` = `(14,125)` при 320x200 (`docs/FILM.md` §1.3/§2/§3);
* `<I x y a>`: `a=l` → левый край, `a=r` → `width-128`, без `a` → `x`
  (`docs/UI.md` §5.2; `docs/HUD_BIND.md` §2.3);
* HUD-тик — счётчик 91 Гц, кадр `tick * fps / 91 + 1` по модулю `count`
  (`docs/HUD_PLAYER.md` §2);
* палитра фильмов — `Film/Kflm0000.bmp` (256 RGBA-квадов, A игнорируется);
* `AMMO4` = `ammo.fdx:4`, 131 кадр при 132 тайлах, `fps = 24`;
* экран `ammo4.sml` не содержит `<I>`, поэтому его фильм — оверлей
  `(14,125)`, а не слот.

**Гипотеза:**

* что *дизайн-страница* `WEAPON15`/`AMMO4` — это страница, которая вызывает
  `film_set` со своим семейством (совпадение имён 1:1 и механизм гаража
  `0x496d82`, но per-screen xref не восстановлен). Поэтому сводка несёт
  `screen_basis`/`film_basis` и `note`, а `resolve_film` помечает выбор из
  состояния как `Hypothesis`;
* что `<I>`-слоты — якоря HUD-фильма (в поставке `<I>` ссылается на loose BMP,
  не на фильм): это контракт `hud_bind`, а не поведение движка;
* что `a=l/r` отсчитывается от краёв экрана, а не от текущей текстовой
  колонки (`0x68e654/0x68e64c`, `docs/UI.md` §10.5);
* что `0x81c4a0` — слот оружия, а не тип боеприпаса (`docs/FILM.md` §9.1).

## 6. Тесты

`engine/tests/test_hud_compose.cpp`:

* **поставка** — `DESIGN/ammo4.sml`, тики 0/91/182: сводка
  (`family=ammo`, `film=AMMO4`, `line=4`, `count=131`, `fps=24`,
  `frame=1/25/49`, `placements=1`, `visible=1`, `clipped=0`, `drawn=1`),
  golden FNV и побайтовое равенство с `hud_player`;
* **`DESIGN/equip0.sml`** — отказ с именем `EQUIP0`, буфер не изменён;
* **`help.sme`** — fallback `Default.sof`, `film_basis = hypothesis`;
* **синтетика** (временный `Film/`): два `<I>` (явный `x/y` и `a=r`), проверка
  пикселей в обоих слотах и в промежутке; target 128x64 — 2 placements,
  1 visible, 1 clipped, 1 drawn; путь без расширения и с неверным регистром
  (`AMMO4.sml`, `Ammo4`) находит `ammo4.sml`; пустой экран, битый `<d>`,
  отсутствующий файл, `EQUIP0` без `.sof`, отрицательный тик
  (`static_cast<uint64_t>(-1)`), граница `UINT64_MAX/15 + 1`, target 64x64,
  короткий буфер, палитра на 255 квадов — везде ошибка и **неизменный**
  target.

Сборка и прогон:

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_hud_compose_tests -j 32
ctest --test-dir build/sandbox -R hud_compose --output-on-failure
python3 regress.py
```

ASan/UBSan: `zigcxx` из `~/tools` **не линкует** ASan
(`undefined symbol: __asan_register_elf_globals` — в комплекте Zig нет runtime
ASan для gnu). Проверка выполнена отдельной сборкой `build/asan` на
g++-14, распакованном без root через `apt-get download`/`dpkg-deb -x`
(`g++-14`, `gcc-14`, `libstdc++-14-dev`, `libgcc-14-dev`, `libasan8`,
`libc6-dev`, `linux-libc-dev`, `libisl23`, `libmpc3`):
`-fsanitize=address,undefined -fno-omit-frame-pointer`, `GM_WITH_SDL3=OFF`.
`gm_hud_compose_tests` прошёл с `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1` — ни одного отчёта.

## 7. Лимиты и trust

* Проверки до записи: target (размер, буфер), палитра, документ, binding,
  фильм, тик, layout. После первой записи отказов быть не должно; если
  `blit_indexed` всё же откажет, ошибка называет индекс placement'а, но часть
  кадра уже нарисована (документировано, не проверяется тестом).
* Никаких аллокаций под размер экрана сверх `HudPlacement[]` (≤ 32) и одного
  `std::vector` с байтами экрана; фильм и палитра приходят готовыми.
* Границы документа/фильма задают `parse_sml` (511 тегов),
  `parse_binding` (±4096, 32 слота), `parse_sof`/`parse_fdx`
  (`kSofMaxTiles`, `kFdxMaxLines`, `kFdxMaxCount`, `kFdxMaxFps`).
* Ничего не создаётся и не изменяется в каталоге игры; временные фикстуры —
  в `std::filesystem::temp_directory_path()/gm_hud_compose_game`.

## 8. Что осталось

1. **Прозрачность и масштаб.** Сейчас фиксированы `transparent = false`,
   `scale = 1`, `opacity = 1` (паритет с golden). Для фронтенда нужна
   `HudComposeOptions` (ключ по индексу 0, zoom 1..4, opacity) — интерфейс
   готов принять её без изменения конвейера.
2. **Loose BMP вместо фильма в `<I>`.** `layout` трактует `<I>` как якорь
   фильма (гипотеза `hud_bind`). Настоящий пейджер рисует по `f=` BMP из
   `FRONTEND/BMP.IDX`; совмещение «BMP + фильм в одном слоте» — отдельная
   задача (нужен `TexturePool`/`res/bmp` и правило, что чем перекрывается).
3. **Интеграция в `main.cpp`.** Модуль не подключён к главному циклу: нет
   источника `HudState` (слоты `0x81c4a0`) и момента отрисовки поверх
   `render_scene`. Это следующий шаг после появления состояния игрока.
4. **Движковый шаг `ms = 91 // fps`.** Как и в `hud_player`, используется
   рациональная формула; для побитового паритета с `0x49fd40` нужен режим
   `ms = 91 // fps` (сейчас расходится при `fps`, не кратном 91, напр. 24).
5. **Палитра по фильму.** В поставке несколько `Kflm*.BMP`; сейчас палитра —
   аргумент вызывающего. Правило выбора (`Kflm0000` для HUD) не проверено
   для всех фильмов.

## CLI

`gunmetal --level NAME --render OUT --hud-screen SML/design/ammo4.sml --hud-tick 0`
прогоняет весь конвейер (`parse_sml → parse_binding → resolve_film →
frame_at → layout → blit`) и печатает family/film/line/frame/placements.
Это `--render`-путь; интерактивный viewer пока использует явный `--hud NAME`.

---

# Часть II. Панельный слой кокпита `level0` (M4.4.5)

* **Статус:** эталонная композиция нижней панели `320x78` в Python:
  `tools/re/hud_compose_ref.py`. Часть I выше (`.sml`/`.sof`-оверлей,
  `hud_compose.*`) **не менялась** — это другой слой HUD: фильмы `128x64`, а
  не панель кокпита `320x78`.
* **Ветка:** `feat/hudcompose`, база `e78dc30`, дата 2026-10-23.
* **Игра:** только чтение (`FRONTEND/BMP.IDX`, кадры `out/oracle/win*.png`).
  Ничего не пишется, `gm/`, `engine/`, `regress.py`, `ROADMAP.md`, `AGENTS.md`,
  `NEXT.md`, `docs/FORMATS.md`, `docs/HUD_AUDIT.md` не менялись.
* **Инструмент:** `python3 tools/re/hud_compose_ref.py` (`--self-test`,
  `--json`, `--png`, `--diff out/oracle/win2.png`).
* **Эталон/смежные документы:** `docs/HUD_AUDIT.md` (инвентарь 23 элементов и
  измеренные bbox), `docs/TEXT_LAYOUT.md` (таблица шрифтов), `docs/PLAYER_FIELDS.md`
  (поля игрока), `docs/ORACLE_RENDER.md` (приведение кадра к 320x200).

## 1. Что именно собирается и в каком порядке

`compose(state) -> (320, 78, bytes RGB)` рисует **панель локально**; при
наложении на кадр её строки кладутся в `y = 122..199`. Порядок слоёв повторяет
вызовы `0x4926a0`:

1. `PANEL` из `FRONTEND/BMP.IDX` — 320x78, индекс 0 — ключ прозрачности;
2. заливки шкал (`memset` цветом `0xf3` = GAMEPAL 243 = чистый красный):
   тяга 4 строки от `(202,155)`, полосы гексагона `(146,150,21x3)` и
   `(147,176,20x3)`;
3. иконки 24x24 (`WICON*`/`WCON*`) в `(23,161)` и `(201,161)`;
4. текст — **после** заливок, поэтому надпись ложится поверх красной шкалы.

Координаты в инструменте — экранные (`320x200`), слой возвращается обрезанным
по `y=122`. Начало текста — **нижний левый угол клетки**: `0x47faf0(font, text,
buf, x, y, pitch, 0)`, строка 0 атласа (низ клетки) попадает на `y`, верхние
строки — на `y-1, y-2, ...`. Это проверено по кадрам:

| вызов | константа кода | ink в кадре `win2` |
|---|---|---|
| `DAMAGE` | `(141,150)` @ `0x4928c6` | `(141,143,34,5)` |
| `KILL` | `(52,186)` @ `0x49289f` | `(52,179,16,6)` |
| имя оружия | `(23,162)` @ `0x492927` | `(23,155,60,8)` |

## 2. Ассеты и шрифты (что доказано)

| элемент | ассет | как рисуется | поле состояния |
|---|---|---|---|
| панель, LED-полосы, гексагон | `FRONTEND/BMP.IDX:PANEL`, 320x78 | блит в `(0,122)`, индекс 0 — ключ | — |
| `MACHINE GUN` / `NO WEAPON`, `KILL`, `DAMAGE`, `THROTTLE`, имя экипировки | `FONTS3` (**шрифт 6**) | клетка 8x8, origin `(8,0)`, шаг 16x16, обрезка по чернилам, `advance = ширина + 1` | слот оружия `+0x3294` → `0x80FC78+slot*0x2A8`, слот экипировки `+0x3320` → `0x814c8c+idx*0x40` |
| патроны | `NUMFONTS` шрифт 10 | клетка 8x10, origin `(0,22)`, копия всей клетки, `advance = 9`; выравнивание вправо по `x=85` (порог 10/100/1000/10000 в `0x4929f4..0x492a6b`) | `ammo[+0x329C] / 0x818980[type]` |
| урон `%` | `NUMFONTS` шрифт 9 (зелёный) / 10 (красный при >=70) | цифры вправо до `x=164` (`x = 166 - 9*len`), `%` — отдельной клеткой в `(166,190)` | `100 - 100*+0x3248/+0x3244` |
| `KILL` значение | `NUMFONTS` шрифт 10 | `(74,185)` | источник не найден (гипотеза) |
| число экипировки | `NUMFONTS` шрифт 10 | `(246,182)` | поле не локализовано (гипотеза) |
| тяга | заливка | `round(throttle*53)` px, строки `155..158` | `engine_power +0x2F48`, форсаж `flags +0x3200 & 2` |
| полосы `DAMAGE` | заливка | `round(charge*21)` / `round(charge*20)` px | `charge_accumulated +0x2FA8` (гипотеза) |
| иконка оружия | `WICON1` 24x24 | блит в `(23,161)`, без ключа | слот оружия `+0x3294` (гипотеза) |
| иконка экипировки | `WICON7` 24x24 | блит в `(201,161)` | слот экипировки `+0x3320` (гипотеза) |

Ключевые уточнения к `docs/HUD_AUDIT.md` и `docs/TEXT_LAYOUT.md`:

1. **Шрифт 6 существует.** В `docs/TEXT_LAYOUT.md` §2.3 сказано «шрифты 6 и
   13..30 не инициализируются», но в листинге есть `push 6` перед
   `call 0x47fe30` (`0x490279`). Аргументы совпадают со шрифтом 7 (FONTS3,
   8x8, 32..95, шаг 16x16, пробел 4, обрезка), отличается только `origin_y`:
   у 6 это 0, у 7 — 32. Без этой записи подписи кокпита не собрать.
2. **`NUMFONTS` — 11 клеток, а не 10.** В зелёной полосе (origin y=6) сразу
   после десяти цифр лежит ячейка `%` (`x = origin_x + 10*16 = 168`), хотя
   таблица объявляет `count = 10`, коды 48..57. Символ `%` в кадре —
   `(166,182,10,9)`, то есть это отдельная клетка атласа, а не шрифт 5.
3. **`WCON*` и `WICON*` перепутаны местами в аудите.** Визуальная сверка
   атласа: `WICON1` — золотая лента патронов (`MACHINE GUN` в `win2`),
   остальные `WICON*` — гранаты/огонь/лампа; `WCON*` — иконки снаряжения и
   powerup'ов. Поэтому по умолчанию берутся `WICON1` / `WICON7`.
4. **`PANEL` — плоский арт той же раскладки, но не тот растр, что в кадре.**
   Геометрия совпадает при `(0,122)`: LED-полосы (панель `y=8..14` → кадр
   `130..136`, `y=74` → `196`), гексагон `(136,17,45,53)` → `(136,139,...)`,
   левый столбик LED `(11,41,6,21)` → `(11,163,6,20)`. А цвета — нет:
   в игре панель стоит на **3D-кокпите**, и металл подсвечен до ~40..80, тогда
   как плоский арт держит рампу 23..143. Отсюда низкий `corr` по фону и
   нормальный — по тексту.

## 3. Сверка с кадром `win2`

Кадр приводится к 320x200 так же, как в `docs/ORACLE_RENDER.md`: crop 1 px
рамки окна → `oracle_compare.resize_area` (area average, обратная операция к
растяжению dgVoodoo) → нижние 78 строк. Метрики по блокам (`corr` — Pearson
яркости, `mad` — 0..1, `edges` — Jaccard Sobel-краёв, `bright` — пиксели
`max(R,G,B)>=70`, `ink` — `>=150`):

```
block        corr      mad    edges   bright(ours/oracle)  ink
all          0.3203   0.1467   0.2388     6280 / 3572     2085 / 1237
left         0.2941   0.1537   0.2840     2112 / 1337      841 / 436
center       0.1727   0.1219   0.2328     1303 / 617       233 / 128
right        0.3830   0.1589   0.2183     2684 / 1633     1012 / 677
top         -0.0556   0.1174   0.0445     1540 / 48        319 / 0
middle       0.4030   0.1533   0.3230     3381 / 2805     1573 / 1039
bottom       0.0474   0.1648   0.2188     1359 / 719       193 / 198
```

Читать это надо так:

* **`middle`/`right` — 0.40/0.38**: там подписи, цифры и шкалы; их геометрия
  совпадает, яркость — частично (шрифт тоньше оригинала на 1 px).
* **`top` — −0.06 и `bright 1540 / 48**: верхняя полоса `PANEL` (ряды 0..19)
  в игре закрыта 3D-панелью и почти не видна; у нас она нарисована целиком.
  Это главный источник расхождения по фону.
* **`all` 0.31** — среднее между совпавшим текстом и несовпавшим фоном;
  это не «провал», а честная граница между 2D-слоем и 3D-кокпитом.

Кластеры (та же функция `hud_audit.clusters`: 8-связность, мост 1 px,
`min_area=12`):

* маска эталона — `static & bright` по `win1..win3`, в полосе `y>=122`
  получается **20 кластеров** (два кластера вне полосы — мир, они
  отбрасываются);
* маска нашей композиции — `ink` (`>=150`), **20 кластеров**;
* каждый эталонный кластер отнесён к элементу той же таблицей
  `hud_audit.match_elements`; **20 из 20** элементов имеют ink в нашей
  композиции (это и есть «элемент нарисован», а не «bbox совпал»);
* медиана IoU bbox нашего ink против bbox эталонного кластера — **0.10**:
  низкая, потому что в эталоне соседние элементы склеены мостом 1 px в блоки
  `(23,155,60,31)` и `(201,152,78,34)`, а у нас ink-кластеры раздельные.
  По отдельным элементам IoU заметно выше: `value_equipment` 0.90,
  `led_left_column` 0.75, `label_kill` 0.67, `value_kill` 0.62,
  `value_damage` (`%`) 0.42, `icon_weapon` 0.28, `icon_equipment` 0.22.
* **19 из 23** элементов дают ink в своих прямоугольниках (у `bar_throttle`
  это ink надписи: сама заливка пуста при `throttle=0`); `bar_damage_top` /
  `bar_damage_bottom` пусты при `charge=0` (как и в кадре, где видны только
  статичные линии), `icon_damage` (силуэт машины) и `compass` не нарисованы —
  см. §4.

Проверка инструмента: `--self-test` — **22/22** (детерминизм, границы
состояния, композиция без ассетов, кламп тяги 53 px, загрузка `PANEL` и
шрифта 6, «состояние меняет картинку»).

## 4. Что осталось непознанным

1. **Иконки 24x24.** `WICON1`/`WICON7` — лучшие визуальные кандидаты, но
   побитового соответствия нет: в кадре иконки лежат на 3D-квадах с
   перспективой (~28x27), а NCC по плоскому атласу даёт всего 0.1..0.3.
   Нужен xref выбора записи по `weapon_slot` / индексу экипировки и модель
   квада, а не атласный блит.
2. **`icon_damage`** (силуэт машины в гексагоне) — это отдельный спрайт,
   которого нет в `PANEL`: в плоском арте на его месте стоят скобки и
   «ножницы». Ассет не найден.
3. **Источник `KILL`** и **число экипировки** — поля не локализованы
   (`docs/HUD_AUDIT.md` §3.3/3.4). В инструменте это параметры состояния.
4. **Компас** (`yaw +0x0028`, полоса `(76,0,54,20)`) — вне панели, рисуется
   динамически; слой `320x78` его не содержит.
5. **Полосы гексагона** — `charge_accumulated +0x2FA8` и координаты заливки
   остаются гипотезой; в `win2` они пусты, поэтому проверены только «пустые».
6. **Анимация LED** (`0x491ec9`: `(opacity+0x80)>>4`, кламп 15, таблица
   `0x682f90`, кадры `BLINK0..3`) — статичный кадр панели берётся из `PANEL`,
   без мигания. Верхние LED-полосы в маске `static & bright` не появляются
   именно поэтому.
7. **3D-подсветка панели.** Плоский `PANEL` в игре освещён иначе; правило
   (множитель/шейдер) не восстановлено. Пока `compose` отдаёт арт как есть.

## 5. План переноса в `gm/render.py`

Ничего из этого в текущей задаче не делалось (`gm/` не менялся).

1. **Модуль.** Перенести тело `hud_compose_ref.py` в `gm/hud_panel.py`
   (или в `gm/render.py` секцией `# --- HUD ---`), оставив публичный API
   `compose(state, assets) -> (320, 78, bytes)` и `compose_indices`.
   `FontSpec`/`Font` — новый маленький `gm/textdraw.py`; спеку шрифтов 6,
   9, 10, 11, 12 взять из §2 (шрифт 6 добавить в `docs/TEXT_LAYOUT.md`).
2. **Ассеты.** Загрузку `PANEL`/`FONTS3`/`NUMFONTS`/`WICON*` делать через
   `gm/bigfile.py` + `gm/bmp.py`; палитра — `gm/palette.installed()`
   (GAMEPAL, не палитра BMP: индекс 0 в ней чёрный, поэтому ключ панели
   совпадает с чёрным). Кэш ассетов держать рядом с `TexturePool`.
3. **CLI `--hud`.** В `gm/render.py` после отрисовки мира:
   ```python
   if args.hud:
       w, h, layer = hud_panel.compose(state, assets)
       blit_rgb(frame, 0, 122, layer)   # панель непрозрачна, ключ уже применён
   ```
   Порядок: мир → `PANEL` → заливки → иконки → текст. Компас — отдельным
   слоем выше (`y=0..20`), после появления привязки к `yaw`.
4. **Состояние.** Пока рантайма нет — брать из sidecar/флагов
   (`--hud-ammo`, `--hud-damage`, `--hud-throttle`, `--hud-kills`,
   `--hud-equipment`). Поля для будущего рантайма: `armor +0x3248`,
   `armor_max +0x3244`, `ammo +0x329C`, `weapon_slot +0x3294`,
   `engine_power +0x2F48`, `flags +0x3200`, экипировка `+0x3320`,
   фары `0x68cd9c`.
5. **Тесты.** `--self-test` уже покрывает детерминизм/границы/отсутствие
   ассетов; в `regress.py` добавить golden FNV композиции для состояния
   `win2` (после переноса) и сравнение «состояние → меняется ровно один
   блок» (ammo/kill/damage/throttle). Сверку с кадром оставить в
   `tools/re/hud_compose_ref.py --diff`, а не в `regress.py` (нужны кадры).
6. **Порядок работ:** (а) перенос без иконок (18 элементов), (б) шрифт 6 в
   `TEXT_LAYOUT`, (в) иконки после xref, (г) компас после `yaw`,
   (д) сведение `--hud` с `oracle_compare` (полоса `hud` станет осмысленной
   только после (а)).

## 6. Границы инструмента

* Не запускает Ghidra: адреса и константы взяты из листинга
  (`tools/re/disrange.py`, `~/.cache/gunmetal-re/gundx.asm`).
* Не пишет в игру/кадры; `--png` пишет только явно указанный файл.
* Без игры `compose` не падает: возвращает чёрный слой с заливками
  (палитра — серая рампа), это проверяет `--self-test`.
* `--diff` ищет кадр по явному пути; если относительного пути нет (worktree
  без `out/oracle`), имя файла разрешается в `out/oracle` основного
  репозитория через `git rev-parse --git-common-dir`.
