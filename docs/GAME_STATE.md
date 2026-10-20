# Gunmetal — C++-слой игрового состояния (`GameState`)

Что здесь: `engine/src/game/state.{h,cpp}` — одна точка между
`res/save.*` (диск), `player/controller.*` (runtime-игрок) и
`world/doors.*` (runtime-двери). Слой загружает SAVE-блоки 9 (player), 2
(doors) и 3 (things) в value-объект, применяет player/doors к runtime-структурам,
собирает обратно в `Save` и проверяет значения. Глобального состояния нет, игра только читается.

Статус: API, mapping, тесты и golden'ы готовы; CTest `game_state` (метка
`quick`, ~0.18 с) проходит. `regress.py` не менялся.

---

## 0. Коротко

| что | значение |
|---|---|
| игрок | блок 9, первый present-слот `0x3510` |
| двери | блок 2, `25` записей в `saved0/saved1` |
| things | блок 3, `6` записей, opaque |
| уровень | `SaveHeader::level_name` (`LEVEL0`) |
| счётчики | `F0..F21`, `F1` — level clock |
| запись | `update_save(Save&)` / `to_save()`, байт-в-байт при no-op |
| чужие блоки | не трогаются, остаются в `Save::blocks` |
| ошибки | чужой уровень, нет блока 9, нет present-слота, OOB/NaN |
| время | ~0.18 с на оба поставочных сейва + level0 `.sbt/.sbw` |

---

## 1. Что переиспользуется

Слой ничего не парсит сам:

* `gm::parse_save` / `Save` / `SavePlayer` / `SaveDoor` / `SaveThing` /
  `Save::to_bytes`
  (`engine/src/res/save.*`, docs/SAVE_CPP.md, SAVE_WRITER.md, SAVE_BLOCKS.md);
* `gm::PlayerState`, `gm::Vec3d` (`engine/src/player/controller.h`);
* `gm::WorldState`, `gm::Door`, `gm::DoorState` (`engine/src/world/doors.h`,
  docs/DOORS_CPP.md).

`GameState` хранит копию исходного `Save` как шаблон для `to_save()`.
Неизвестные блоки (8, 11, 12, 15–18, 20, ...) остаются в `Save::blocks` и
копируются `Save::to_bytes` дословно; типизированно переписываются только
блоки 9, 2 и 3.

---

## 2. API

```cpp
#include "game/state.h"

std::string error;
std::optional<gm::Save> save = gm::parse_save(bytes, error);
auto state = gm::GameState::from_save(std::move(*save), "level0", error);

gm::PlayerState player = gm::make_state({0, 0, 0}, 0.0);
state->apply_to_player(player);          // pos + yaw, vel = 0

gm::WorldState world;
world.build(sbt, sbw);
const std::size_t matched = state->apply_to_doors(world); // 25 для LEVEL0

// запись обратно в тот же контейнер (неизвестные блоки сохраняются)
state->update_save(*save);
auto out = save->to_bytes(error);
```

| метод | смысл |
|---|---|
| `static std::optional<GameState> from_save(Save, std::string_view level, std::string&)` | загрузка; `level0`, `LEVEL0`, `level0.sbt` эквивалентны |
| `bool validate(std::string* = nullptr) const` | значения и диапазоны без мира |
| `bool apply_to_player(PlayerState&, std::string* = nullptr) const` | `pos`, `yaw`, сброс `vel`/`on_ground` |
| `std::size_t apply_to_doors(WorldState&, std::string* = nullptr) const` | применение по `(sector, object)`, число совпадений |
| `bool update_save(Save&, std::string* = nullptr) const` | запись в существующий `Save` |
| `std::optional<Save> to_save(std::string&) const` | копия шаблона + `update_save` |

Поля: `level_name`, `fields`/`level_clock`, `position`, `angles`,
`armor_max/armor`, `shield_max/shields[4]`, `weapon_slot/weapon_mask`,
`ammo[12]`, `history_index`, `exists`, `status_present`, `object_id`, `doors`,
`thing_present[260]`, `things`.

`level_clock` — авторитетный счётчик: при записи он копируется в `fields[1]`.

---

## 3. Mapping save → runtime

### 3.1. Игрок (блок 9, struct `+0x0000..`)

| save | `GameState` | runtime |
|---|---|---|
| `struct+0x00` 3×f64 | `position[3]` | `PlayerState::pos` (feet point) |
| `struct+0x18` 3×f64 | `angles[3]` | `PlayerState::yaw = angles[2]` |
| `struct+0x3244/0x3248` | `armor_max/armor` | HUD/будущий chassis (пока только данные) |
| `struct+0x324c/0x3250` | `shield_max/shields[4]` | HUD/будущий chassis |
| `struct+0x3294/0x3298` | `weapon_slot/weapon_mask` | выбор оружия |
| `struct+0x329c` 12×u32 | `ammo[12]` | боезапас |
| `struct+0x3204/0x3210/0x3214/0x3178` | `history_index/status_present/object_id/exists` | служебные поля |

Позиция сейва — та же мировая точка, что и `PlayerState::pos`: спавн
`SET_PLAYER_START[0] = (24, -208, 48)` в docs/PLAYER.md §4 трактуется
контроллером как feet point, и `saved0/saved1` дают `z = 48.0`.
`angles[0]/[1]` — pitch/roll (0 в обоих сейвах), `angles[2]` — yaw
(docs/CAMERA.md §6).

### 3.2. Двери (блок 2, запись `0xB8`)

| save | `Door` |
|---|---|
| `+0x00..0x30` 9×f64 | `closed[3]`, `open[3]`, `size[3]` |
| `+0x68` flags | `flags`, `kind = flags & 0xf` |
| `+0x6c` 5×i32 | `ids[5]`, `linked = ids[3] != 0` |
| `+0x80` state | `state` (`-1/0/+1` → `Closing/Closed/Opening`) |
| `+0x84/0x88` | `sector/object` |
| `+0x8c/0x90` | `phase_prev/phase_next` |
| `+0x94` step | `step` |
| `+0x48` delta, `+0x60` const, `+0x98/0x9c`, `+0xa0/0xac` | сохраняются в `GameState::doors`, runtime их не использует |

Сопоставление идёт по **`(sector, object)`**, а не по `Door::id`: в level0
несколько дверей делят один GENERATE-id (`307` встречается 12 раз), тогда как
адрес створки уникален. Совпавшие двери получают `flags/ids/closed/open/size/
step/state/phase`; `phase = phase_next`. Runtime-поля `locked`, `timer`,
`hold_ticks` в сейве отсутствуют и не меняются.

### 3.3. Things (блок 3)

Запись `0x528` не имеет проверенной карты полей, поэтому `GameState` хранит
`thing_present[260]` и `std::vector<SaveThing>` (slot + сырые `0x528` байт)
дословно. Runtime-приёмника для things в C++ пока нет, поэтому
`apply_to_things` не существует: слой гарантирует загрузку, валидацию
(слоты `0..259`, ascending, совпадение с флагами) и запись без потерь.

### 3.4. Заголовок

`level_name` — `SaveHeader::level_name`; `fields` — все 22 счётчика;
`level_clock = fields[1]` (4288 / 8148).

---

## 4. Запись обратно

`update_save`:

1. `validate()`;
2. проверяет тот же уровень (нормализация регистра/пути/`.sbt`);
3. требует блок 9 и present-слот;
4. пишет типизированные поля первого present-слота;
5. `header.fields = fields`, `fields[1] = level_clock`;
6. если в `Save` есть блок 2 — заменяет `Save::doors` на `GameState::doors`
   (если двери есть, а блока 2 нет — ошибка);
7. если в `Save` есть блок 3 — записывает `thing_present`/`things`
   (если things есть, а блока 3 нет — ошибка).

`Save::to_bytes` сам собирает блок 9 из `SavePlayer` и блок 2 из
`Save::doors`; остальные блоки идут из `SaveBlock::raw`. Поэтому
`parse → from_save → update_save → to_bytes` при отсутствии правок даёт
исходный файл байт-в-байт, включая блок 19 (`ITEM`) и прочие непонятые блоки.

---

## 5. Ошибки и границы

`from_save` возвращает `nullopt` и текст в `error`:

| условие | пример текста |
|---|---|
| уровень не совпал | `level mismatch: save is "LEVEL0", requested "level1"` |
| нет блока 9 | `save has no player block (9)` |
| блок 9 без present-слота | `player block has no present slot` |
| битый контейнер | ошибка `parse_save` (до `GameState`) |

`validate()` дополнительно отвергает: пустое/слишком длинное имя уровня,
отсутствие игрока, NaN/inf в позиции/углах, `armor_max`/`shield_max` вне
`1..10000`, `armor`/`shields` вне максимума, `weapon_slot` вне `0..11`,
биты `weapon_mask` выше 11, `> 100` дверей, отрицательные
`sector/object`, `state` вне `-1..1`, `phase` вне `0..65536`,
отрицательный `step`, не-конечные `box/delta/constant`.

`apply_to_player`/`apply_to_doors`/`update_save` сначала вызывают
`validate()`, поэтому невалидное состояние не доходит ни до runtime, ни до
файла.

---

## 6. Golden'ы (поставочные сейвы)

`/mnt/d/games/Gunmetal 3dfx/SAVEGAME/`, оба файла 243368 байт.

| поле | `saved0.sbt` | `saved1.sbt` |
|---|---|---|
| FNV-1a64 файла | `0xb994929515f53af5` | `0x0d038197e152cd65` |
| level | `LEVEL0` | `LEVEL0` |
| level clock `F1` | 4288 | 8148 |
| position | 1116.173751898651, 798.2076214059642, 48.0 | 1156.355385156283, 797.2758300679517, 48.0 |
| angles | 0, 0, 5.0736149946795805 | 0, 0, 4.747052494679579 |
| history_index | 194 | 130 |
| armor / max | 10000 / 10000 | 10000 / 10000 |
| shields / max | 8000 ×4 / 8000 | 8000 ×4 / 8000 |
| weapon slot / mask | 0 / 3 | 1 / 3 |
| ammo[0], ammo[1] | 2000, 90 | 1975, 87 |
| doors | 25 | 25 |
| things (слоты) | 6 (0..5) | 6 (0..5) |

Дверные golden'ы (одинаковы в обоих сейвах):

| запись | flags | ids | state | sector/object | phase prev/next | step | param5 |
|---|---|---|---|---|---|---|---|
| 0 | 3 | `2,0,0,0,0` | 0 | 4 / 1 | 0 / 0 | 700 | 300 |
| 20 | 2 | `0,7,0,-30238,-30239` | 0 | 48 / 6 | 0 / 0 | 800 | 800 |
| 24 | 3 | `0,32,0,0,0` | 0 | 54 / 1 | 0 / 0 | 800 | 400 |

Проверено тестом `game_state`:

* оба сейва: загрузка, golden-поля, `validate()`, no-op
  `update_save → to_bytes` байт-в-байт (и `to_save()` тоже);
* правка `position[0] += 1.25`, `ammo[0] += 7`, `doors[0].state = 1`,
  `phase_next = 32768` читается обратно после `to_bytes → parse_save`, при
  этом `armor`, `doors[20]` и прочие блоки не меняются;
* `apply_to_player` даёт точные `pos`/`yaw`, нулевую скорость;
* `apply_to_doors` на реальном `level0.sbt/level0.sbw` находит 25/25 дверей,
  golden'ы `step/state/phase/sector/object`, полуоткрытая дверь
  (`phase = 32768`) даёт `open_fraction = 0.5`.

Синтетика: сохраняется контейнер с блоками `2 + 9 + 19 (ITEM, 0x7080
байт)`, проверяется round-trip без изменений, применение к игроку и к
`WorldState`, собранному из такого же `DOOR`-record, и правка с сохранением
opaque-блока.

---

## 7. Proven / hypothesis / trust

**Proven (по коду игры и docs/SAVE_FIELDS.md, SAVE_CPP.md, SAVE_BLOCKS.md,
DOORS_CPP.md):**

* смещения и типы полей блока 9 и записи двери `0xB8`;
* `position` — мировая позиция, `angles[2]` — yaw;
* `(sector, object)` — адрес створки; `state -1/0/+1`; `phase 0..65536`;
* структура контейнера, порядок блоков, отсутствие checksum;
* блок 3 (things) — `260` флагов + `0x528` на present-слот;
* no-op сериализация байт-в-байт.

**Гипотеза:**

* `phase_next` — текущая фаза после последнего тика (в сейвах обе фазы 0,
  поэтому выбор не влияет на golden; `phase_prev` сохраняется в
  `GameState::doors` и в файле без изменений);
* сброс `vel`/`on_ground` при загрузке — в сейве скорости нет;
* `level0`, `LEVEL0`, `level0.sbt` — один и тот же уровень (нормализация);
* `weapon_mask` — биты `0..11` (в сейвах 3).

**Trust:** C++-тест сверяет golden'ы с уже проверенными
`test_save*.cpp`/docs и реальными файлами; `apply_to_doors` — с
`level0.sbt/level0.sbw`. Ошибки парсинга остаются за `parse_save`, слой их
не маскирует.

---

## 8. Сборка и тесты

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_game_state_tests -j 32
ctest --test-dir build/sandbox -R game_state --output-on-failure
```

CTest `game_state` — метка `quick`, ~0.18 с (без установленной игры
real-часть печатает `skipping` и не падает). `python3 regress.py` не
редактировался и проходит: `113/113 checks passed` (~82 с).

---

## 9. Остаток

* `world/doors.h` не даёт mutable-доступа к `doors()`; `apply_to_doors`
  использует `const_cast` (объект не const). Правильное решение — публичный
  setter/mutable-доступ в `doors.h`, но этот файл вне scope задачи.
* Нет runtime-потребителей `armor/shields/ammo/weapon` и `GameState` в
  `main.cpp` (main не менялся): слой готов, но ещё не подключён к игровому
  циклу.
* `SavePlayer` типизирует только первый present-слот; остальные слоты и
  attachments сохраняются дословно, но `GameState` их не редактирует.
* Скорость/`on_ground`/высота капсулы не сохраняются (в формате их нет).
* Блок 3 (things) загружается и пишется обратно, но runtime-приёмника нет;
  блок 11 (SCRIPT runtime) остаётся полностью opaque.
* Пустые `doors`/`things` при наличии блока в `Save` уходят в fallback
  `Save::to_bytes` на исходный `raw` (так устроен writer); очистка списка
  через `GameState` не поддерживается.
