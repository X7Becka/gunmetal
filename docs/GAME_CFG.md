# `GUNMETAL.CFG` в движке (`M12.5`/`M1.5`, срез 1)

**Статус:** модуль `engine/src/game/game_cfg.{h,cpp}` + тест `game_cfg`
(`engine/tests/test_game_cfg.cpp`, CTest label `quick`, **624 проверки**).
Читает 16 доказанных полей из `docs/CONFIG.md` §2/§3, пишет побайтовую копию
в выбранный путь и никогда — в каталог игры. Не подключён к `main.cpp`,
`GameRuntime`, меню и CLI; значения никуда не применяются.

Это **игровой** конфиг (`GUNMETAL.CFG`), не CLI-конфиг движка
(`engine/src/core/config.h`: `-g/--game`, `GUNMETAL_DIR`) — их не путать.

Проверка:

```sh
cmake --build build/sandbox --target gm_game_cfg_tests -j 4
./build/sandbox/engine/tests/gm_game_cfg_tests
ctest --test-dir build/sandbox -R '^game_cfg$' --output-on-failure
python3 regress.py --mirror --json
```

Установка читается только на чтение (`/mnt/d/games/Gunmetal 3dfx` или зеркало
`~/.cache/gunmetal-re/game`); скретч-файлы теста идут в `/var/tmp`
(`GM_GAME_CFG_TMPDIR` переопределяет).

## 1. Доказано (код игры + данные поставки)

Источники — `docs/CONFIG.md` §1–§3 (адреса `gundx.exe`, ImageBase `0x400000`).
Все поля — little-endian `u32`; смещения от начала файла.

| факт | значение | источник |
|---|---|---|
| размер | ровно **8192 (0x2000)**; `fread(buf, 0x2000, 1, f)` | `0x484e8a` |
| магия | 37 байт `"GUNMETAL Configuration File. av1.93"` + `0x1A` + NUL | `0x484e9b` |
| версия | сравнивается целиком, `av1.93` — часть формата | `0x484e9b`…`0x484ed2` |
| поля | 0x190…0x1DC, писатель кладёт их обратно без изменений | `0x485440`, §3 |
| «мусор» | 0x025…0x18F и 0x8AC…0x1FFF писатель не инициализирует | §2.1 |

Поля в модуле (16; 13 изменяемых):

| смещ. | поле | поставка | допустимо | доказательство |
|---|---|---|---|---|
| 0x190 | `music_volume` | 16 | 0..32 `MODEL` | `'Music Volume:'` `0x4bd508`, полоса `0x488a4f` |
| 0x194 | `sfx_volume` | 20 | 0..32 `MODEL` | `'Effects Volume:'` `0x4bd4f8`, `0x488a9d` |
| 0x19C | `swap_left_right` | 0 | 0..1 | `'Switch Left Right'` `0x4bd4e4`, тумблер `0x48be13` |
| 0x1A0 | `width` | 800 | read-only | getter `0x486440`, таблица режимов `0x4bfd18` |
| 0x1A4 | `height` | 600 | read-only | тот же getter |
| 0x1A8 | `colour_depth` | 8 | read-only | сравнение с 8 в `0x446975` |
| 0x1B4 | `border` | 3 | 0..9 | `'Border:'` `0x4bcf1c`, `cmp 9` в `0x48c773` |
| 0x1B8 | `gamma` | 1 | 0..3 | `'%s Gamma Correction'` `0x4bcf08`, 4 записи `0x4bc6d8` |
| 0x1C0 | `reflections` | 1 | 0..1 | `'Reflections'` `0x4bcefc`, чекбокс `0x485fc0` |
| 0x1C4 | `light_maps` | 2 | 0..2 | off/low/high `0x4bcebc`/`0x4bcee4`/`0x4bcecc` |
| 0x1C8 | `dynamic_camera` | 1 | 0..2 | none/some/full `0x4bcf38`/`0x4bcf50`/`0x4bcf68` |
| 0x1CC | `play_flags_p1` | 0x10400 | биты `0x11D400` | §3.1, маска `0x44d68c` |
| 0x1D0 | `play_flags_p2` | 0x10400 | биты `0x11D400` | вторая копия, фронтенд меняет обе |
| 0x1D4 | `deathmatch_type` | 0 | 0..2 | таблица `0x4bc6e8` |
| 0x1D8 | `starting_cash` | 7 | 0..11 | таблица `0x4bc6f8` (12 значений) |
| 0x1DC | `deathmatch_flags` | 4 | биты `0x75` | §3, `0x488646`… |

Сквозные факты, которые проверяет тест на реальном файле:

* `GUNMETAL.CFG` — 8192 Б, FNV-1a 64 `439e3e13a3242648`, байт 0x23 = `0x1A`,
  байт 0x24 = `0x00`;
* значения из таблицы читаются и из структуры, и как `u32` из `raw` по тому же
  смещению;
* `raw` → `game_cfg_bytes()` → **8192/8192 байт, 0 отличий**; запись в
  скретч-копию и повторная загрузка дают те же байты и те же поля;
* `set_field` меняет только 4 байта своего поля: `gamma 1→3` — 1 байт в
  0x1B8, `border 3→9` — 1 байт в 0x1B4, `play_flags_p1` — 2 байта в 0x1CC и
  **не трогает** `play_flags_p2` в 0x1D0;
* границы: конец диапазона принимается, «на единицу дальше» отвергается с
  текстом (`outside`), биты вне маски — с текстом `bits outside 0x11d400` /
  `bits outside 0x75`.

## 2. Модель (`MODEL`)

1. **Диапазон громкостей 0..32.** Доказано только имя и то, что полоса
   рисуется как `v * 2.7` (`0x488a4f`/`0x488a9d`); что UI ограничивает
   значение 32 — не доказано. Поэтому 0..32 помечено `MODEL`; загрузка
   значение вне диапазона не отвергает (файл читается как есть), ограничение
   действует только в `set_field`.
2. **Read-only `width`/`height`/`colour_depth`.** Игра заполняет их из таблицы
   режимов `0x4bfd18` (`0x486440`), «легального» значения для редактора нет,
   поэтому `set_field` их отвергает (`read-only`), а чтение и round-trip
   работают.
3. **Запись в каталог игры запрещена.** `save_game_cfg` сравнивает
   канонизированный родительский каталог с `GUNMETAL_DIR`,
   `/mnt/d/games/Gunmetal 3dfx` и `~/.cache/gunmetal-re/game` (зеркало
   `regress.py`) и отказывает **до** открытия файла. Это правило движка: сама
   игра пишет туда при выходе (`0x4908b2`), но движок обязан оставить
   оригинал неизменным.
4. **`set_field` не трогает `raw`.** Правка живёт в полях структуры;
   `game_cfg_bytes()` накладывает их на копию `raw`. Поэтому неизменённый
   образ кодируется побайтово, а правка затрагивает ровно 4 байта поля.
5. **Поля вне модели.** `marker` (0x198, загрузчик принудительно ставит 1,
   `0x484fc2`) не поле, а инвариант; `unknown_1bc` (0x1BC) не читается никем;
   `refresh` (0x1AC) и `mode_flags` (0x1B0, кроме бита 0x800) — гипотезы
   (§11). Ни одно из них в `GameCfg` не попало, `set_field` их отвергает как
   `unknown field`. Поля `shadows` в `docs/CONFIG.md` **нет**; ближайшее
   доказанное — `reflections`.
6. **`av1.93` — единственная версия.** `CONTROLS.CFG` (`av1.91`) отвергается
   по шапке, а не разбирается «по полям».

## 3. API (`game/game_cfg.h`)

```cpp
struct GameCfg {
    std::string header;            // 35 байт магии, без 0x1A/NUL
    std::vector<std::uint8_t> raw; // все 8192 байта дословно
    int music_volume, sfx_volume, swap_left_right, width, height, colour_depth,
        border, gamma, reflections, light_maps, dynamic_camera,
        play_flags_p1, play_flags_p2, deathmatch_type, starting_cash,
        deathmatch_flags;
};

const std::vector<GameCfgField>& game_cfg_fields();      // 16 записей
const GameCfgField* find_game_cfg_field(std::string_view name);

bool parse_game_cfg(std::span<const std::uint8_t> bytes, GameCfg&, std::string* error);
bool game_cfg_bytes(const GameCfg&, std::vector<std::uint8_t>& out, std::string* error);
bool load_game_cfg(const std::filesystem::path& file, GameCfg&, std::string* error);
bool save_game_cfg(const std::filesystem::path& file, const GameCfg&, std::string* error);
bool set_field(GameCfg&, const std::string& name, int value, std::string* error);
std::string game_cfg_version(const GameCfg&);            // "1.93"
```

Контракт ошибок: `false` + непустой текст, начинающийся с `GUNMETAL.CFG:`.
Отказ ничего не меняет: `parse`/`load` не трогают переданный `GameCfg`,
`set_field` — поле, `save_game_cfg` — файл (проверка каталога идёт раньше
`ofstream`).

Тексты (проверяются подстрокой):

| ситуация | текст |
|---|---|
| размер 0/8191/8193/16384 | `…: <N> bytes, expected 8192` |
| битая магия | `header "<…>" is not "GUNMETAL Configuration File. av1.93" + 0x1a 0x00` |
| терминатор | `header terminator 0x1b at 0x23, expected 0x1a` |
| NUL | `header byte 0x24 is 0x41, expected 0x00` |
| нет файла | `GUNMETAL.CFG: no such file: <путь>` |
| поле вне диапазона | `<поле> = 10 is outside 0..9 (CONFIG.md §3 …)` |
| бит вне маски | `<поле> has bits outside 0x11d400: 0x1` |
| read-only | `field width is read-only (CONFIG.md §3 …)` |
| неизвестное поле | `unknown field "shadows" (proven fields: music_volume, …)` |
| запись в игру | `refusing to write into the game installation <каталог>: <путь>` |
| пустой `raw` | `raw image is 0 bytes, expected 8192` |
| заголовок структуры изменён | `header "<…>" is not …, refusing to encode` |

## 4. Числа теста (`game_cfg`, 624 проверки, 0.15 с)

```
game_cfg: GUNMETAL.CFG 8192 B fnv=439e3e13a3242648 fields=16 round-trip=8192/8192
          music=16 effects=20 800x600x8 border=3 gamma=1 light_maps=2 camera=1
          play=0x10400 cash=7 dm_flags=0x4
game_cfg: table fields=16 settable=13 read_only=3
game_cfg: set_field fields=13 changed_bytes=15 max_per_field=2
          music_volume=16->0:1B sfx_volume=20->0:1B swap_left_right=0->1:1B
          border=3->0:1B gamma=1->0:1B reflections=1->0:1B light_maps=2->0:1B
          dynamic_camera=1->0:1B play_flags_p1=66560->1168384:2B
          play_flags_p2=66560->1168384:2B deathmatch_type=0->2:1B
          starting_cash=7->0:1B deathmatch_flags=4->117:1B
game_cfg: install guard targets=3 refused, shipped file untouched
game_cfg: CONTROLS.CFG 8192 B av1.91 refused (opaque body, docs/CONFIG.md §8)
game_cfg: 624 checks, ok (GUNMETAL.CFG 8192 B, 16 fields, 13 settable,
          round-trip 8192/8192 bytes)
```

FNV-1a 64 `439e3e13a3242648` — хэш C++-теста; `c1460f4e` из `docs/CONFIG.md`
§11 — 32-битный хэш `tools/parity_config.py` (другой вариант/длина), это не
расхождение. `CONTROLS.CFG` — 8192 Б, FNV-1a 64 `6a85c7a9a5528336`, магия
`av1.91`.

`regress.py --mirror --json`: `{"passed": 115, "total": 115, ...}`.

## 5. Ограничения

* **Не подключено:** ни `main.cpp`, ни `GameRuntime`, ни фронтенд, ни CLI не
  читают `GUNMETAL.CFG` через этот модуль; значения не применяются к рендеру,
  экрану, звуку и управлению. `--dump`/`--game` его не показывают.
* **Только `av1.93`.** `CONTROLS.CFG` (`av1.91`) и `KEYCFG/*.KEY` не покрыты:
  первый отказывается по шапке (тело не разобрано, `docs/CONFIG.md` §8),
  вторые — другой формат (376 Б, `name[16]` + 45 пар, §9), отдельный загрузчик
  и другой срез.
* **Имена профилей (0x1E0…0x227), джойстик/оси (0x248…0x278) и четыре записи
  контроллера (0x27C…0x8AB)** доказаны, но остаются внутри `raw` как opaque:
  round-trip их сохраняет, типизированных полей у них нет.
* **Поля-гипотезы не редактируются:** `refresh`, `mode_flags` (кроме
  доказанного бита), `unknown_1bc`, `marker`. Ширина/высота/глубина цвета
  читаются, но не пишутся.
* **«Мусорные» области** (0x025…0x18F, 0x8AC…0x1FFF) не интерпретируются. Если
  игра перезапишет файл, они изменятся — это свойство формата, а не парсера.
* **Каталог игры только на чтение:** запись в него отвергается, поэтому
  проиграть конфиг можно лишь копией вне установки.
* Новый (`GameCfg{}`) файл собрать нельзя: `raw` обязателен (8192 Б), иначе
  `game_cfg_bytes`/`save_game_cfg` откажут. Это осознанно: без `raw` нечем
  заполнить 0x025…0x18F и 0x8AC…0x1FFF.

## 6. Что дальше (вне этого среза)

1. Имена профилей 0x1E0…0x227 (`name1[k]`/`name2[k]`, чередование по 8 байт) и
   их сверка с `KEYCFG/*.KEY` — срез управления.
2. Оси/кнопки/чувствительность 0x248…0x278 (мс → секунды, «пол» 0.2 с,
   `0x4a2910`/`0x4a2930`).
3. Четыре записи контроллера 0x27C…0x8AB (`0x18C` на запись, 32 канала,
   маски/пресеты/биты).
4. Применение значений к движку (громкости, гамма, border, режим) — только
   после того, как появятся потребители в C++ (сейчас их нет).
5. `CONTROLS.CFG` — отдельная задача: без exe версии 1.91 тело недоказуемо,
   `docs/CONFIG.md` §8 фиксирует это как opaque.
