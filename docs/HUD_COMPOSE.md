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
