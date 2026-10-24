# HUD-иконки и компас: C++ (`id -> FRONTEND/BMP.IDX`, геометрия ленты)

* **Статус:** срез 2 `M4.4.5`. Модуль отвечает на два вопроса: какой ассет
  `FRONTEND/BMP.IDX` соответствует записи `DEFINE_WEAPONS` / `DEFINE_EQUIPMENT`
  и где на экране стоит азимут на ленте компаса. Рисования нет: позиции блита
  `(23,170)`/`(201,170)`, направление строк и компоновка остаются вызывающему.
* **Ветка:** `feat/hudicons2`, база `master` `68e652c`, дата 2026-10-24.
* **Игра:** только чтение (`FRONTEND/BMP.IDX`, `level0.sbt`); ничего не
  пишется. Синтетические BIGFILE-пары теста лежат в системном tmp.
* **Файлы:** `engine/src/render/hud_icons.h`,
  `engine/src/render/hud_icons.cpp`, `engine/tests/test_hud_icons.cpp`; строка
  `src/render/hud_icons.cpp` в `engine/CMakeLists.txt`, цель
  `gm_hud_icons_tests` / тест `hud_icons` (label `quick`) в
  `engine/tests/CMakeLists.txt`.
* **Эталон:** `docs/HUD_ICONS.md` §1–§5 (главный источник чисел),
  `docs/FORMATS.md` §1/§12.3, `docs/UI.md` §7.1 (`BMP.IDX`),
  `docs/PLAYER_FIELDS.md` (`+0x3320`/`+0x3328`, `+0x0028`).

`render/hud.*`, `render/hud_player.*`, `res/bmp.*`, `main.cpp`, `config.*`,
`ROADMAP.md` не менялись.

## 1. Что доказано, а что модель

| факт | база | доказательство |
|---|---|---|
| имя иконки оружия — третий строковый операнд `DEFINE_WEAPONS` (опкод 170), он же `strings[2]` в C++ `Weapon::icon` | **доказано** | `0x450c97` (`push 0x18/0x18/0`, dest `record+0x50`, имя, `"frontend"`, `call 0x47efe0`); `0x450c35` (имя для подписи `record+0x298`); C++ `Weapons::load` читает те же записи |
| **формулы** `id -> WICONn/WCONn` нет: имя лежит в данных записи | **доказано** | 36 `.sbt`, 684 записи, 19 видов, у каждого вида всегда один и тот же ассет; `tools/re/hud_icons.py --icons` |
| `WICON*` и `WCON*` — оба **иконки оружия** (не экипировки) | **доказано** | §1.3: `VIRAL AGENT → WCON41`, `ROCKET → WCON18` и т. д.; исправление к `docs/HUD_COMPOSE.md` §2 п. 3 |
| порядок `level0.sbt`: 19 записей, `MACHINE GUN, CANNON, AUTOCANNON, RAILGUN, GRENADE, VIRAL AGENT, FLAMETHROWER, ROCKET, RGM, TAC NUKE, MRL, LASER, FUSIONBEAM, PLASMA CANNON, MICROWAVE GUN, EMP GUN, DISRUPTORFIELD, DISINTEGRATOR, MESON GUN` | **доказано данными** | `Weapons::load(level0.sbt)` (тест сверяет все 19 пар имя/ассет) + `tools/re/hud_icons.py` |
| запись содержит **один** растр 24×24 (`+0x50`, `0x240` байт) | **доказано** | `0x450c82..0x450c9c`, §1.1–§1.2 |
| иконка экипировки — `sprintf("equip%d", i)`, 12 слотов HUD | **доказано** | `0x48fa6f` (формат `0x4be3a8`), `0x492b32`, `0x492cc3` (`cmp eax,0xc`), §2 |
| `EQUIP12` (`BALLAST`) есть в архиве и в `DEFINE_EQUIPMENT`, но вне окна HUD | **доказано данными** | §2.2; тест: 13 записей, `equip12` 24×24 |
| обе иконки — 24×24, 24 байта в строке | **доказано** | `0x450c97`/`0x48fa86` (`push 0x18/0x18`) |
| `x = 102.62 + 0.2940*(bearing - yaw)`, окно `x 79..129`, `y 2..15` | **MODEL** | подгонка по 6 рискам трёх кадров `win1..win3`, rms 1.08 px; **код отрисовки не найден** (§5, §7.3) |

Следствия для API:

* `weapon_icon()` принимает **id записи**, а не «вид оружия по номеру»: имя
  берётся из таблицы поставки (`kWeaponIconTable`, порядок `level0.sbt`).
  Для уровня с другим порядком/подмножеством id вне `0..18` — отказ текстом,
  а не догадка.
* `alt=true` — отказ: «альтернативной» иконки в записи нет, `WICON*`/`WCON*`
  принадлежат разным оружиям.
* `equipment_icon()` реализует формулу §2 (`equip0..equip12`), потому что
  формула доказана.
* Компас помечен `kCompassBasis = "MODEL (...)"`; это не доказанный код.

## 2. API

```cpp
enum class IconKind { weapon, weapon_alt, equipment, ammo, unknown };

struct IconRef {
    IconKind kind;
    std::string asset;          // "WICON1" из скрипта, "equip0" из sprintf
    int width, height;          // из заголовка BMP (в поставке 24x24)
    int index_in_archive;       // 0-based позиция в BMP.IDX
};

struct Arch {                   // read-only, имена без учёта ASCII-регистра
    std::filesystem::path idx_path;
    std::shared_ptr<const BigFile> file;
    static std::optional<Arch> open(const std::filesystem::path& bmp_idx,
                                    std::string& error);
    bool empty() const;  std::size_t size() const;
    const BigEntry* find(std::string_view name) const;
    int index_of(std::string_view name) const;
    std::optional<BmpImage> image(std::string_view name, std::string& error) const;
};

// 19 видов в порядке level0.sbt (имя записи + ассет).
std::span<const WeaponIconSlot> weapon_icon_table();

std::optional<IconRef> icon_asset(std::string_view asset, IconKind kind,
                                  const Arch& bmp, std::string* error);
std::optional<IconRef> weapon_icon(int weapon_id, bool alt, const Arch& bmp,
                                   std::string* error);
std::optional<IconRef> equipment_icon(int equip_id, const Arch& bmp,
                                      std::string* error);

struct CompassGeometry {        // §5: origin_x, px_per_deg, yaw_deg, y, окно
    double origin_x = 102.62, px_per_deg = 0.2940, yaw_deg = 0.0;
    int y = 2, window_left = 79, window_right = 129;
};
struct CompassPose { double bearing_deg; int x, y; };
CompassPose compass_pos(double bearing_deg, const CompassGeometry& geometry);
bool compass_in_window(int x, const CompassGeometry& geometry);
```

Контракт отказов (все — текст в `*error`, `nullopt`; `error` очищается при
успехе):

| вход | текст содержит |
|---|---|
| id оружия вне `0..18` | `outside the shipped level0 DEFINE_WEAPONS table` |
| `alt=true` | `no alternate icon` |
| id экипировки вне `0..12` | `outside equip0..equip12` |
| пустой `Arch{}` | `BMP archive is empty` |
| ассета нет в архиве | `asset 'WICON1' is not in ... (N entries)` |
| блоб не BMP | `is not a readable 8-bit BMP` |
| пустое имя ассета | `icon asset name is empty` |
| `Arch::open` без индекса | текст `BigFile::open`: `cannot open index` |

`Arch{}` — валидный **пустой** архив, поэтому негативы не требуют игры.
`Arch::open` не копирует `.dat`: чтение идёт через `BigFile::read`, размеры
берутся из заголовка BMP (`parse_bmp`), а не из константы 24.

Компас: азимут нормализуется в `[0,360)` (`450 -> 90`, `-90 -> 270`), дельта
до `yaw` сворачивается в `[-180,180)`, `x` округляется `lround`. `x` может
быть вне окна — клипит вызывающий (окно видит ~173.5°). Нефинитный вход даёт
`kCompassInvalid` в `x`/`y` (не UB через приведение). `y` — верхняя строка
окна (`2`); лента имеет фиксированную высоту.

## 3. Числа теста

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx -DGM_WITH_SDL3=OFF
cmake --build build --target gm_hud_icons_tests -j 4
./build/engine/tests/gm_hud_icons_tests
ctest --test-dir build -R hud_icons --output-on-failure
```

Прогон 2026-10-24 (установка `/mnt/d/games/Gunmetal 3dfx`):

```
compass: yaw=344.61 bearing=0   -> x=107 y=2 (inside)
compass: yaw=344.61 bearing=90  -> x=134 y=2 (outside)
compass: yaw=344.61 bearing=180 -> x=54  y=2 (outside)
compass: yaw=344.61 bearing=270 -> x=81  y=2 (inside)
shipped: archive=/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX entries=143
weapon: id=0 name="MACHINE GUN" asset=WICON1  24x24 archive_index=126
weapon: id=1 name="CANNON"      asset=WICON3  24x24 archive_index=128
weapon: id=2 name="AUTOCANNON"  asset=WICON4  24x24 archive_index=129
weapon: id=3 name="RAILGUN"     asset=WICON5  24x24 archive_index=130
weapon: id=4 name="GRENADE"     asset=WICON6  24x24 archive_index=131
weapons: resolved=19/19 24x24=19 unresolved=0 []
equipment: id=0 asset=equip0 24x24 archive_index=24
equipment: id=1 asset=equip1 24x24 archive_index=25
equipment: id=2 asset=equip2 24x24 archive_index=29
equipment: resolved=12/12 24x24=12 unresolved=0
equipment: id=12 asset=equip12 24x24 (archive-only, outside 0x492cc3)
determinism: id=17 asset=WCON11 index=92 twice equal
hud_icons: 296 checks, 0 failed
hud_icons: OK (icons from FRONTEND/BMP.IDX, compass MODEL)
```

Что именно проверено (`296` проверок):

* **таблица = данные:** все 19 пар `kWeaponIconTable[i]` совпадают с
  `Weapons::load(level0.sbt)`; у каждого id `weapon_icon()` даёт
  `asset == Weapon::icon`;
* **независимое чтение:** для всех 19 иконок тест заново берёт запись из
  `BigFile`, читает блоб и парсит `parse_bmp`; `width/height` из `IconRef`
  равны размерам читателя (19/19 по 24×24);
* **экипировка:** формула `equip%d` (регистр не важен: в архиве `EQUIP0`),
  индексы `equip0=24, equip1=25, equip2=29`, `equip12=28`; 12 слотов HUD и
  `BALLAST` — 24×24;
* **компас:** yaw `344.61` (`win2`) — числа выше; 2 из 4 кардиналов попадают
  в окно 51 px; период 90° = 26.46 px; `360==0`, `450==90`, `-90==270`,
  `765 -> 45`; yaw 0: `0->103, 90->129, 180->50, 270->76`; NaN/`+inf` →
  `kCompassInvalid`;
* **негативы:** id `-1`/`999`, `alt=true`, `equip13`, пустой архив,
  `WICON1` в архиве без него, блоб не BMP, отсутствующий `BMP.IDX`;
* **детерминизм:** повторный `weapon_icon(17)` даёт тот же `IconRef`
  (`WCON11`, индекс 92);
* **синтетика:** BIGFILE-пара собирается тестом вручную (2×3 BMP) — проверка
  размеров/индекса/регистронезависимого поиска без игры.

Сверка с формулой `docs/HUD_ICONS.md` §5: `bearing 270` при yaw `344.61`
даёт `80.68 -> 81` (в кадре измерено 81), `bearing 0` — `107.14 -> 107`
(в кадре 109, разница 2 px внутри rms подгонки 1.08 px). Числа кадра тест не
читает: он проверяет **модель**, а не `win2.png`.

## 4. Ограничения

* **Не рисует** в кадр и не знает палитры: возвращает ассет, размер и индекс
  записи. Блит 24×24 — `blit_indexed`/следующий срез; позиции `(23,170)` и
  `(201,170)`, обратный проход строк у оружия и прямой у экипировки
  (`docs/HUD_ICONS.md` §3) сюда не входят.
* **Нет анимации, подсветки и приоритетов.** Рамка-дизеринг `0xAA` по маске
  `player+0x3328` (`0x492d26`) и порядок слоёв — вне среза.
* **Компоновка** панели (`hud_compose` / `0x4926a0`) — отдельный срез; здесь
  только данные иконки и геометрия ленты.
* **`alt` не поддержан осознанно:** в записи один растр; вторая иконка
  оружия не выдумывается. `IconKind::weapon_alt`/`ammo` зарезервированы.
* **Таблица оружия — `level0.sbt`.** Другие уровни могут иметь другое
  подмножество/порядок (`DEFINE_WEAPONS` нумеруется заново на блок), поэтому
  id вне `0..18` — отказ; для них нужна своя таблица из их `.sbt`.
* **Компас — MODEL.** Адрес отрисовки не найден; `y` фиксирован (верх окна),
  вертикальная структура рисок/букв не моделируется, перспектива (шаг рисок
  6..9 px) не учтена. `kCompassBasis` печатается рядом с числами.
* **3D-слой кокпита не адресуется:** почему видимые в кадре иконки на 7–8 px
  выше 2D-блита (`docs/HUD_ICONS.md` §7.1), этот модуль не объясняет.
* **`+0x3320`** используется как номер экипировки вслед за HUD; спор
  «`fire_anim` vs номер» (`docs/HUD_ICONS.md` §7.2) не закрыт и на API не
  влияет: `equipment_icon` принимает уже готовый индекс.

## 5. Проверки

```sh
./build/engine/tests/gm_hud_icons_tests               # 296 checks, 0 failed
ctest --test-dir build -R '^hud_icons$' --output-on-failure
python3 tools/re/hud_icons.py --self-test            # 48/48 (тот же источник чисел)
python3 regress.py --mirror --json                   # Python-оракул, см. отчёт
```

Без установки игры shipped-половина печатает
`hud_icons: game not installed, shipped cases skipped`, синтетическая и
компасная части выполняются всегда.
