# Рендер экрана 10 `CONTROL OPTIONS` (`engine/src/ui/control_options_render.h`)

Срез 17 пункта `M8.8`: один кадр экрана **10** фронтенда (`gundx.exe`, ImageBase
`0x400000`) в RGBA-буфер 320×200. Модуль без состояния — рисует то, что ему
передали в `ControlOptionsRenderOptions`, и отдаёт числа (`rows`, `sliders`,
`checks`, `arrows`, `ink_pixels`, `missing_glyphs`). Шрифты, метрики и атласы —
из `render/text_layout` (`FONTS4` в `FRONTEND/BMP.IDX`), точный fade — из
`render/hud_lut`. Ничего не измеряется заново.

Код: `engine/src/ui/control_options_render.h/.cpp`; тест —
`engine/tests/test_control_options_render.cpp` (CTest `control_options_render`,
label `quick`). Соседние срезы: `PLAYER_SETUP_RENDER.md` (36),
`MOUSE_SETUP_RENDER.md` (22), `KEYCFG_RENDER.md` (30/34/35), `MENU_RENDER.md`
(общая механика `0x485dd0`).

## 1. API

```cpp
#include "ui/control_options_render.h"

gm::ControlOptionsRenderOptions options;  // defaults = .data/.bss движка (§2.3)
options.cursor = 1;                       // [0x68ccf4], id пункта 0..9
options.keyset1 = 0;                      // [0x5a4910] -> "View %s" (>=1 -> "Assign %s")
options.keyset2 = 1;                      // [0x4aa488] (.data = 1)
options.keyset_names = {"PERFECT", "SPLIT1"}; // name[16] выбранных наборов
options.joystick1 = 0;                    // [0x4bc540], -1 = Not Used
options.joystick_name1 = "SideWinder Precision Pro"; // 0x4bf7f0 + dev*0x7c, "%.25s"
options.mouse_type = 1;                   // [0x5a4938] -> "Mouse: Player 1"
options.background = "PLATE";             // "" = чёрный холст

std::vector<std::uint32_t> rgba;          // 0xRRGGBBAA
int width = 0, height = 0;
gm::ControlOptionsRenderStats stats;
std::string error;
if (!gm::render_control_options(game, options, rgba, width, height, &stats, &error)) {
    // error: текст с префиксом "control_options_render: "
}
// width=320 height=200, rgba.size()==64000
// stats: rows=10 sliders=0 checks=0 arrows=0 ink_pixels=1736 missing_glyphs=0
```

| Поле `ControlOptionsRenderOptions` | По умолчанию | Смысл |
|---|---|---|
| `std::string background` | `""` | запись `FRONTEND/BMP.IDX`, рисуется в (0,0); пусто — чёрный холст |
| `int cursor` | `0` | `[0x68ccf4]`, **id пункта** 0..9; вне диапазона ничего не подсвечивает (`-1` — кадр без подсветки) |
| `int keyset1` | `0` | `[0x5a4910]`, кламп 0..15; `0` → `View %s`, `>= 1` → `Assign %s` |
| `int keyset2` | `1` | `[0x4aa488]`, кламп 0..15 (`.data` = 1) |
| `std::array<std::string,2> keyset_names` | `{"",""}` | `name[16]` **выбранного** набора игрока 1/2 (`0x6c2058 + set*0x108`) |
| `int joystick1` | `-1` | `[0x4bc540]`, `-1` = `Player 1 Joystick: Not Used` |
| `int joystick2` | `-1` | `[0x4bc544]` |
| `std::string joystick_name1/2` | `""` | имя устройства `0x4bf7f0 + dev*0x7c`, кап 25 байт |
| `int mouse_type` | `0` | `[0x5a4938]`, 0..2 → `kControlOptionsMouseNames` |
| `bool fade_lut` | `false` | точный fade `0x682f90` по палитровым индексам |
| `int fade_lut_level` | `11` | 0..15; 11 — тождество, 12..15 светлеют |
| `std::string fade_lut_dump` | `""` | дамп 4096 Б (`load_hud_lut`) |
| `const HudLut* fade_lut_table` | `nullptr` | готовая таблица, приоритетнее дампа |

Публичные помощники (нужны тесту и будущему CLI):
`control_options_row_y(item)`, `control_options_row_x(item)`,
`control_options_title_x(face5)`, `control_options_mouse_name(index)`,
`control_options_item_text(options, item)`.

`ControlOptionsRenderStats`: `rows` (всегда 10), `sliders`/`checks`/`arrows`
(всегда 0 — виджетов и стрелок на экране нет), `ink_pixels` (уникальные пиксели
глифов; фоновая плата не считается — как в `menu_render`), `missing_glyphs`
(байты без глифа, не фатальны).

## 2. Доказано (листинг + данные)

### 2.1. Пролог и геометрия

Слот диспетчера: `eax = [0x4bc440] - 5`, переход по `0x4883d8 + eax*4`; для
id 10 индекс 5 → `0x4883ec` = `0x487ec0`, стаб `call 0x488ba0; jmp 0x487f64`.
Тело — `0x488ba0`..`0x488e83` (`ret`).

| факт | значение | адрес |
|---|---|---|
| `rows` (`[0x67f10c]`) | `0xa` = 10 | `0x488ba3` |
| шаг (`[0x662d58]`) | `9` | `0x488bad` |
| копия платы | `call 0x486050` (196×131 `PLATE`) | `0x488bb7` |
| заголовок | `0x4bd5f4`, шрифт 5, `centered=1`, `x 20` (игнорируется), `row 14`, `item -1` | `0x488bbc`..`0x488bcb` |
| строка пункта | `row = 27` (`push 0x1b`) у всех десяти вызовов | `0x488bff`, `0x488c55`, `0x488c92`, `0x488ce8`, `0x488cfb`, `0x488dab`, `0x488e01`, `0x488e18`, `0x488e53`, `0x488e66` |
| `x` пункта | `0x14` = 20 везде, кроме пунктов 1/3 (`0x20` = 32) | `0x488c57`, `0x488cea` |
| формула `y` | `row >= 0` → `y = row + item*[0x662d58]` | `0x485e4b`..`0x485e5c` |

Отсюда `y = 27 + 9*i`: 27, 36, 45, 54, 63, 72, 81, 90, 99, 108. Заголовок —
абсолютная строка `y = 14` (`0x485e09`: `item == -1` обнуляет `ecx`, ветка
`row < 0` не нужна — `row = 14` уже положительный, поэтому в `0x485dd0` он
складывается с `item*step = 0`).

Центровка заголовка: `0x485de2`..`0x485dfa` — `(196 - width(font 5, text)) / 2`
(деление с округлением к нулю); `push 0x14` в `x` на этой ветке не читается.

### 2.2. Строки: источник текста

| item | текст | VA | адрес сборки |
|---:|---|---:|---|
| 0 | `Player 1: %s` — `name[16]` набора `0x6c2058 + [0x5a4910]*0x108` | `0x4bd5e4` | `0x488bd0`..`0x488c08` |
| 1 | `View %s` (`0x4bd5dc`) при set `< 1`, иначе `Assign %s` (`0x4bd5d0`) | — | `0x488c15`..`0x488c5e` |
| 2 | `Player 2: %s` — набор `[0x4aa488]` | `0x4bd5c0` | `0x488c63`..`0x488c9b` |
| 3 | `View`/`Assign %s` для игрока 2 | — | `0x488ca8`..`0x488cf1` |
| 4 | `Keyjamming` | `0x4bd5b4` | `0x488cf9` |
| 5 | `1: %.25s` (`0x4bd58c`) или `Player 1 Joystick: Not Used` (`0x4bd598`) | — | `0x488d6f`..`0x488db8` |
| 6 | `2: %.25s` (`0x4bd564`) или `Player 2 Joystick: Not Used` (`0x4bd570`) | — | `0x488dbd`..`0x488e0e` |
| 7 | `Joystick Centering` | `0x4bd550` | `0x488e16` |
| 8 | `Mouse: %s` из `0x4bc548 + [0x5a4938]*4` | `0x4bd544` | `0x488e2a` |
| 9 | `Done` | `0x4bceb4` | `0x488e64` |

Правило `View`/`Assign` — `cmp eax,1; jge` (`0x488c15`, `0x488ca8`): `Assign`
только при индексе набора `>= 1`. Имя набора — `name[16]` записи 0x108 байт:
первичные коды `+0x00`, вторичные `+0x7c`, имя `+0xf8` (`0x6c1f60 + 0xf8 =
0x6c2058`, docs/KEYCFG_RENDER.md §2); в `.KEY`-файле имя лежит в первых 16
байтах (`PERFECT.key` → `PERFECT`).

Таблица `0x4bc548` (её же рисует экран 22 в строке `Used By`): 0 — `Not Used`
(`0x4bca50`), 1 — `Player 1` (`0x4bc944`), 2 — `Player 2` (`0x4bc938`);
дальше идут алиасы имён действий, куда `[0x5a4938]` не попадает (`idiv 3` в
`0x48c31b`), поэтому модель клампит 0..2.

### 2.3. Значения по умолчанию (`.data`/`.bss`)

| переменная | значение | источник |
|---|---|---|
| `[0x5a4910]` (набор игрока 1) | 0 | `.bss` |
| `[0x4aa488]` (набор игрока 2) | **1** | `.data` |
| имена наборов `0x6c2058 + set*0x108` | пусто | `.bss` до загрузки `KEYCFG/*.KEY` |
| `[0x4bc540]` / `[0x4bc544]` (джойстики) | `-1` / `-1` | `.data` (`0xffffffff`) |
| `[0x5a4938]` (мышь) | 0 = `Not Used` | `.bss` |

В поставке три набора — `PERFECT.key`, `SPLIT1.KEY`, `SPLIT2.KEY` (по 376 Б);
при старте загрузчик раскладывает их в записи `0x6c1f60 + set*0x108` и копирует
имя 16 байтами (`push 0x10` в `0x48f95c`, docs/UI_SCREENS.md §7). Модуль эти
файлы **не читает** — имена приходят в `keyset_names`.

### 2.4. Подсветка и выход

`0x485dd0` (`0x485e0d`..`0x485e19`): если `item == [0x68ccf4]` **и** шрифт
вызывающего равен 0, шрифт становится 4. Собственной раскраски строк на экране
10 нет (в отличие от 36/22), поэтому белой становится ровно строка курсора.
Заголовок идёт с `item = -1` и не подсвечивается никогда.

Esc обрабатывает общий путь `0x48e4ca`: состояния 16/22/26/30/34/36 уходят
обратно в 10 (`0x48e599`), 18 → 12 (`0x48e55a`), а 10 не сравнивается ни с чем
и попадает в ветку по умолчанию `[0x4bc440] = 1` (`0x48e58c`) — выход в главное
меню. Пункт 9 (`Done`) идёт общим путём выбора `0x48e49c`, который пишет
состояние вызывающего; в кадре это никак не участвует.

### 2.5. Ассеты и виджеты

`FRONTEND/BMP.IDX`: `PLATE` 196×131 (копия в `0x6865f8`) и `FONTS4` — грани 5
(заголовок), 0 (пункты), 4 (курсор). Список ассетов экрана в §2 — `plate` +
шрифты, и это подтверждается телом обработчика: в `0x488ba0`..`0x488e83` нет
вызовов `0x486070` (`SLIDE`), `0x4860b0` (`KNOB`), `0x4833e0`
(`CHECK`/`NOCHECK`), `0x485cc0`/`0x485d00` (`MENUARRW`) и помощников окна
списка. Все десять строк влезают в плату (`108 + 8 = 116 < 131`), скролла нет.

## 3. Модель (и чем она отличается от факта)

* Значения полей — из `ControlOptionsRenderOptions`; всё, что обработчик читает
  из глобалов, здесь параметр. Имена наборов — строки, а не запись 0x108.
* Клампы: индексы наборов 0..15 (§7), индекс мыши 0..2, имена — NUL-обрезка и
  кап 16/25 байт (`strncpy`/`%.25s`). Индекс набора влияет только на слово
  `View`/`Assign`: имя берётся из `keyset_names`.
* Джойстик считается присутствующим при слоте `>= 0`. Оригинал перед печатью
  проверяет бит 0 байта `[0x4bf7e0 + dev*0x7c]` и при его отсутствии сбрасывает
  слот в `-1` и чистит `[0x5a4918 + dev*4]` (`0x488d0d`..`0x488d65`) — это
  побочный эффект DirectInput-перечисления, в кадре он не воспроизводится.
* Фон: `background == ""` — чёрный холст; `"PLATE"` — плата в (0,0). В
  оригинале плата всегда скопирована в `0x6865f8`, поэтому ассет валидируется
  даже при пустом фоне.
* Цвета — из палитр атласа: шрифт 5 → (155,15,15), 0 → (19,15,15), 4 →
  (239,239,239) (docs/MENU_RENDER.md §3). Глифы у граней 0, 4, 5 одинаковые,
  поэтому разница «курсор/не курсор» равна чернилам строки (§4).
* Клип — 196×131 (окно копии платы); за пределами не рисуется ничего.
* Fade — тот же путь индекс→индекс через `render/hud_lut.h::map` перед
  выборкой цвета; без таблицы/дампа вызов отказывает, а не рисует «как без
  fade».

## 4. Числа (`ctest -R '^control_options_render$'`, 425 проверок)

Кадр по умолчанию (курсор 0, наборы 0/1 без имён, оба джойстика `Not Used`,
мышь `Not Used`):

```
control_options default: rows=10 sliders=0 checks=0 arrows=0 ink=1736 missing=0
```

Заголовок: шрифт 5, ширина 83, x = (196-83)/2 = **56**, y = **14**,
bbox (56,14)-(137,20), ink 233.

Строки (x, y, ink, bbox):

```
Player 1: "                  x= 20 y= 27 ink= 85 bbox (20,27)-( 54,34)
View "                       x= 32 y= 36 ink= 58 bbox (32,36)-( 52,42)
Player 2: "                  x= 20 y= 45 ink= 96 bbox (20,45)-( 57,52)
Assign "                     x= 32 y= 54 ink= 83 bbox (32,54)-( 58,61)
Keyjamming                   x= 20 y= 63 ink=143 bbox (20,63)-( 69,70)
Player 1 Joystick: Not Used  x= 20 y= 72 ink=287 bbox (20,72)-(138,79)
Player 2 Joystick: Not Used  x= 20 y= 81 ink=298 bbox (20,81)-(141,88)
Joystick Centering           x= 20 y= 90 ink=205 bbox (20,90)-( 99,97)
Mouse: Not Used              x= 20 y= 99 ink=187 bbox (20,99)-( 92,105)
Done                         x= 20 y=108 ink= 61 bbox (20,108)-(39,114)
```

`233 + (85+58+96+83+143+287+298+205+187+61) = 1736` — сумма чернил совпадает
с `stats.ink_pixels` (строки не перекрываются).

Кадр с реальными именами из установки (`PERFECT`/`SPLIT1` из `KEYCFG/*.KEY`),
джойстиком 1 и мышью `Player 1`: `ink=2083`, `missing=0`; строки
`Player 1: PERFECT` (ink 198), `View PERFECT` (171),
`Player 2: SPLIT1` (168), `Assign SPLIT1` (155),
`1: SideWinder Precision Pro` (287), `Mouse: Player 1` (164).

Значения меняют только свои строки (полный diff кадра):

```
keyset1 0->2          99 px   (пункты 0..1; между ними и ниже пункта 1 — 0)
keyset_names          370 px  (пункты 0..3)
joystick1             409 px  (только пункт 5)
joystick2             350 px  (только пункт 6)
mouse_type 0->2       132 px  (только пункт 8)
```

Курсор: `cursor = i` отличается от `cursor = -1` ровно на чернила строки
`i` (85, 58, 96, 83, 143, 287, 298, 205, 187, 61) и ни на пиксель вне её
полосы; `cursor = 10` побайтово равен `cursor = -1`.

`PLATE` против чёрного холста: 23 940 из 25 676 пикселей платы свободны от
текста и совпадают с независимой раскодировкой `PLATE`; diff = 23 940 px,
`ink_pixels` не меняется (1736), за пределами 196×131 — 0 непустых пикселей.

Fade (плата + курсор 2):

```
уровень 11 vs без fade: 0 / 64000 px
12/13/14/15 vs без fade: 16128 / 24226 / 24465 / 25560
соседние 12-13, 13-14, 14-15: 17262 / 22666 / 20837
уровень 13 детерминирован (0 отличий при повторе)
```

Минимальный архив `FONTS4 + PLATE` (без игрушек и стрелок) рендерится:
`rows=10 sliders=0 checks=0 arrows=0 ink=1736`.

## 5. Ограничения

* Нет ввода: клавиши, мышь, джойстик, Esc, перемещение курсора, вход в
  подэкраны (`0x48e888`: 0/2 листают набор, 1/3 — в набор, 4 — Key Jamming,
  5/6 — джойстик игрока, 7 — центрирование, 8 — mouse setup, 9 — выход) — всё
  вне модуля.
* **CLI — есть (срез 19), звука нет.** `--control-options
  [--control-options-cursor N] [--control-options-keyset1 N]
  [--control-options-keyset2 N] [--control-options-joystick1 N]
  [--control-options-joystick2 N] [--control-options-mouse N] [--no-plate]
  [--png OUT] [--fade-lut N]`: по умолчанию
  `control_options: rows=10 sliders=0 checks=0 cursor=0 arrows=0 ink=1736
  missing=0`; набор 2 + джойстик 1 + мышь 2, курсор 9 — `ink=1472`. CTest
  `cli_control_options*`; `cli_fade_lut` (11 = без fade). Звуки `updown`/
  `select` (`docs/UI_SCREENS.md` §8) не моделируются.
* Настройки не читаются и не пишутся: `GUNMETAL.CFG` (`[0x5a4910]`,
  `[0x4bc540]`, ...), `KEYCFG/*.KEY` и DirectInput-перечисление остаются
  снаружи; имена и слоты приходят параметрами.
* Не воспроизводятся побочные эффекты `0x488d0d`..`0x488d65` (сброс битого
  слота джойстика и `[0x5a4918 + dev*4]`) и `0x484b10` (пересборка
  `0x83c980`/`0x83ca00` — валидация наборов клавиш).
* Виджетов нет: `SLIDE`/`KNOB`/`CHECK`/`NOCHECK`/`MENUARRW` не рисуются и не
  запрашиваются у архива, `sliders`/`checks`/`arrows` всегда 0.

## 6. Проверка

```sh
cmake --build build/sandbox --target gm_control_options_render_tests -j 4
ctest --test-dir build/sandbox -R '^control_options_render$' --output-on-failure
# control_options_render: ok (425 checks)
python3 regress.py --mirror --json
```

Тест печатает заголовок/строки (x, y, ink, bbox), числа статистики, diff
значений, курсора, платы и fade; без установки игры пиксельная половина
пропускается (`game not installed`), модельная и негативная части идут всегда.
