# `.soe` → `THING_TYPES`: C++ reader (`engine/src/res/soe.*`)

C++-порт грамматики `gm/soe.py`: текстовый `.soe` читается в `SoeFile` /
`ThingType` с картой 77 полей 936-байтовой записи `THING_TYPES`, байтовым
round-trip и лимитами. Игра только читается; `engine/src/main.cpp` и
`text.cpp` не менялись (старый `gm::Soe` из `res/text.h` остаётся потоком
токенов для CLI).

## 1. Корпус

В поставке **один** `.soe`: `SOBS/railrock.soe` (340 байт, 61 строка CRLF,
67 токенов, версия 13). Проверено `find`/`glob` по `SOBS/`; других `.soe` в
установке нет. Поэтому:

* golden — точные 340 байт `railrock.soe` и все 67 значений, снятые
  Python-парсером;
* остальные ветки грамматики (версии 0..13, `none`/`sphere`/`ellipsoid`,
  path/weapons/children) закрыты 14 синтетическими фикстурами, каждая
  разобрана `gm/soe.py` с `extra == 0`, а C++-тест сверяет число полей и
  FNV-1a64 канонического дампа значений;
* при установленной игре тест дополнительно читает реальный `SOBS/railrock.soe`
  и сверяет его с встроенным golden (байты и round-trip).

`DEFINE_THING_TYPES` в `.soe` **нет**: это не директива формата. Строки
`DEFINE_*` в `gundx.exe` — `DEFINE_AMMO`, `DEFINE_EQUIPMENT`,
`DEFINE_LEVEL_OBJECTIVES`, `DEFINE_PLAYER_TYPES`, `DEFINE_REGENERATION_MARKER`,
`DEFINE_SHIELDS`, `DEFINE_VIRUS`, `DEFINE_WEAPONS`; `THING_TYPES` — имя
бинарного блока `.sbt` (§4 `gm/sbt.py`). `.soe` — просто поток токенов без
разделителя записей, поэтому в `SoeFile` всегда одна запись.

## 2. API

```cpp
#include "res/soe.h"

std::string error;
auto file = gm::parse_soe(bytes, error, "railrock"); // name = stem, optional
if (!file) { /* error */ }

const gm::ThingType& thing = file->thing();
thing.name;          // имя типа (аргумент name / stem файла)
thing.id;            // токен class_name -> индекс SOB_TABLE (+0x1a4)
thing.version;       // 0..13
thing.fields;        // SoeField: label/kind/i/d/s/offset
thing.extra;         // неиспользованные хвостовые токены (0 = всё съедено)
thing.find("f1d4");  // SoeField* или nullptr
thing.get_int("f198");
thing.get_double("radius_z");
thing.get_string("collision_type");

file->to_bytes();    // точные входные байты
```

* `parse_soe(span<const uint8_t>, error, name = {})` — на ошибке `nullopt` и
  текст в `error`; на успехе `error` пуст.
* `SoeField.offset` — смещение в записи `THING_TYPES`; для массивов считается
  `offset + index * stride` (например `weapon[1].w2` → `0x280`). Для
  `version` и вычисляемых полей (`radius`, `collision_kind`) смещения нет
  (`-1`), как и в Python.
* `field_map()` — `std::span<const SoeFieldInfo>` из 77 строк: `offset`,
  `size`, `kind` (`int`/`double`/`name16`), `name`, `role`, `status`
  (`proven`/`hypothesis`/`unknown`), `soe`/`legacy` label, `count`/`stride`,
  `instance`, `loader`, `xref`. `label_info(label)` резолвит и скалярные
  метки, и `weapon[i]`, `child[i]`, `path_point[i]`.
* `kSoeRecordSize == 936` (`0x3a8`).

Лимиты (`kSoeMax*`): 1 МиБ, 65 536 токенов, 4 096 байт на токен, версия
0..13, оружие ≤ 8, дети ≤ 4, точки пути ≤ 32 (последние три — клампы
загрузчика `0x47dc2c`, `0x47ddad`, `0x47daea`).

## 3. Грамматика

Порядок токенов снят с `fscanf` загрузчика `0x47cb6c` и совпадает с
`gm/soe.py`:

| # | Условие | Токены | Поля |
|---|---------|--------|------|
| 1 | всегда | `%ld` | version |
| 2 | всегда | `%ld %ld %s %ld %ld` | f19c, f198, class_name (id), f1a8, f1ac |
| 3 | v ≥ 9 | `%ld` | f1b0 |
| 4 | всегда | `%s %lf×6 %s` | collision_type, col_f10/20/28/90/98/a0, collision_shape |
| 5 | ellipsoid | `%lf×3` | radius_x/y/z |
| 5 | sphere | `%lf` | radius |
| 6 | всегда | `%lf×4` | f50, f58, f60, f68 |
| 7 | v ≥ 7 | `%s %ld` | event_a, f1d4 |
| 7 | v < 7 | `%s %s %ld` | event_a, event_b, f1d4 |
| 8 | v ≥ 8 | `%ld×4` | shield0..3 |
| 8 | v < 8 | `%ld` | shield0 |
| 9 | всегда | `%s %s` | tex_a, tex_b |
| 10 | всегда | `%ld` | expire_value |
| 11 | v ≥ 3 | `%s` | expire_event |
| 12 | всегда | `%s %lf×3 %ld %s %s %ld×3` | str_c8, f78/80/88, f1c8, str_f0, str_1a4, f200/210/214 |
| 13 | v ≥ 4 | `%lf` | falling_fear |
| 14 | v ≥ 5 | `%s` | create_event |
| 15 | v ≥ 6 | `%s×3` | morale_failure_event, time_out_event, damaged_event |
| 16 | v ≥ 7 | `%ld×2` | f1cc, f1d0 |
| 17 | v ≥ 10 | `%lf×2` | f_a8, f_b0 |
| 18 | v ≥ 11 | `%lf×2` | f_b8, f_c0 |
| 19 | v ≥ 12 | `%lf×2` | f18, f70 |
| 20 | v ≥ 13 | `%s %lf` | leash_location, f_d0 |
| 21 | v ≥ 2 | `%ld`, count×`%s` | path_point_count, path_point[i] |
| 22 | всегда | `%ld`, count×(`%s %ld×6`) | weapon_count, weapon[i].name/w0..w5 |
| 23 | v ≥ 1 | `%ld`, count×(`%s %lf×6 %ld`) | children_count, child[i].name/c0..c5/x |

Токены разделяются пробельными символами так же, как `str.split()` для
latin1: `0x09..0x0d`, `0x1c..0x1f`, `0x20`, `0x85`, `0xa0`. Регистр формы
коллизии приводится к нижнему (как Python `.lower()`); `PUSHABLE` в
`railrock.soe` — это движение (`collision_type` → `+0x1b4`), а форма —
отдельный токен `ellipsoid` → `+0x1a0`.

## 4. Карта 77 полей

`field_map()` — построчная копия `FIELD_MAP` из `gm/soe.py`:
**40 proven, 22 hypothesis, 15 unknown**. Смещения от базы записи
`0x776d60`, шаг `0x3a8`. Ключевые точки:

* `+0x000` `name[16]` — имя типа (копируется загрузчиком из аргумента, не из
  `.soe`; в C++ это `ThingType::name`);
* `+0x1a4` `sob_index` — токен `class_name`, резолвер `0x47c9b0` →
  `SOB_TABLE` (в C++ `ThingType::id`); статус proven;
* `+0x1a0` `collision_kind` — токен формы (`none/sphere/ellipsoid`);
* `+0x1b4` `movement_type` — legacy-метка `collision_type`;
* `+0x1d4` `health`, `+0x1d8..1e4` щиты, `+0x1e8` expire, события
  `+0x1b8/1bc/1c0/1c4/1ec/1f0/1f4/1f8`, `+0x1fc` звук;
* массивы: `weapon_id` `+0x21c + 4*i`, параметры `+0x23c..+0x2dc`,
  `child_type` `+0x300 + 4*i`, `child_param` `+0x310 + 4*i`,
  `path_point` `+0x324 + 4*i`, `child_x/y/z` `+0xd8/0xe0/0xe8`, шаг `0x18`.

Значения статусов: proven — роль доказана строкой/потребителем загрузчика;
hypothesis — смещение и ширина доказаны, роль выведена; unknown — известны
только смещение/ширина. Подробные xref — в `docs/SOE.md` §3 и в полях
`loader`/`xref` таблицы.

## 5. Golden и тест

`engine/tests/test_soe.cpp`:

* встроены точные байты `railrock.soe` и 67 строк «label/kind/value» из
  Python; сверяются все значения, порядок, смещения (`class_name` → `0x1a4`,
  `weapon[0].w2` → `0x27c`), имя/id/версия/extra, `to_bytes()`;
* 77/40/22/15, уникальность строк, размеры `int`/`double`/`name16`,
  резолв всех `soe`-меток и массивов, `kSoeRecordSize == 936`;
* 14 фикстур v0..v13: гейты `f1b0`, `event_b`, `shield1/3`, `expire_event`,
  `falling_fear`, `create_event`, morale/timeout/damaged, `f1cc`, `f_a8`,
  `f_b8`, `f18`, leash, path/children, формы `none/sphere/ellipsoid`,
  FNV-1a64 значений;
* негативные: пустой/пробельный файл, все 67 усечений по границам токенов,
  токен 4097 байт, NUL, файл > 1 МиБ, > 65 536 токенов, 9 оружий,
  5 детей, 33 точки пути, версия 14 и −1, отрицательный счётчик,
  нечисловые `%ld`/`%lf`; хвостовые токены принимаются с `extra != 0`
  (паритет с Python);
* при наличии игры — все `SOBS/*.soe` (сейчас 1/1), байтовый round-trip.

Запуск:

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_soe_tests -j 32
ctest --test-dir build/sandbox -R soe --output-on-failure
```

## 6. Отличия от `gm/soe.py`

Намеренные (документированы, на поставочном файле не срабатывают):

1. **`to_bytes()`** возвращает исходные байты как есть. Python собирает
   `lines` через `split("\r\n")` и `join("\r\n")`, то есть нормализует файл
   с LF к CRLF; C++ сохраняет и LF, и отсутствие финального перевода строки.
   Для `railrock.soe` результаты совпадают побайтово.
2. **Границы**: версия обязана быть 0..13; отрицательные счётчики и
   превышение 8/4/32 — ошибка. Python принимает версию любого размера,
   отрицательный счётчик трактует как 0, а большие — читает без клампа
   (загрузчик клампит).
3. **NUL** отвергается с указанием смещения; Python после latin1-декода
   считает его обычным символом токена.
4. **Целые** ограничены `int64` (Python — arbitrary precision); переполнение
   даёт ошибку, а не значение.
5. **`SoeField.offset`** заполняется для всех меток, у которых есть строка
   карты (Python заполняет только явно переданные смещения). Для массивов
   C++ сразу считает смещение элемента.
6. Не портированы `describe()`, `render_lines()` и CLI-обёртка: они
   презентационные; канонический writer новых файлов остаётся в Python.

Общее: порядок токенов и гейты, `str.split()`-набор пробелов, `%s` как
произвольный токен, разбор чисел в десятичном подмножестве Python
(`+1.5`, `.5`, `5.`, `1e3`, `inf`/`nan`), `extra` вместо ошибки на хвост,
`ThingType.id == class_name`. Подчёркивания в числах (`1_0`) Python принимает,
C++ — нет (в поставке не встречаются).

## 7. Доказано / гипотеза / остаток

Доказано (proven, 40): имена/смещения/роли, подписанные загрузчиком и
потребителями — `name`, `sob_index` `+0x1a4`, `collision_kind`, `movement_type`,
`search_radius` `+0x58`, `target_zone_threshold` `+0x1cc`,
`target_history_offset` `+0x1d0`, `max_health` `+0x1d4`, щиты, события
create/expire/damaged/morale/timeout, `sound_id`, `expire_ticks`, текстуры,
счётчики и массивы оружия/детей/пути, радиусы.

Гипотеза (22): `off_z`, `f20/f28`, `col_f90/98/a0`, `f198/f19c`, флаги
`f1a8/f1ac/f1b0`, `event_low_health`/`event_death`/`event_destroy`,
`hits`, `timer_period/step`, `sound_volume`, `weapon_p0..p2`,
`leash_distance`.

Неизвестно (15): `f10`-подобные double (`f18`, `f50`, `f60`, `f68`,
`f70`, `f78`, `f80`, `f88`, `f_a8`, `f_b0`, `f_b8`, `f_c0`) и
`weapon_p3..p5`.

Остаток:

* формулы физики/коллизии для unknown/hypothesis-полей;
* точная семантика шести параметров оружия;
* имена битов `f1a8/f1ac/f1b0` и глобал `0x83d62c`;
* зачем загрузчик умножает `+0x1e8`, `+0x214`, `weapon.w2` на `0.91`;
* writer `.soe` (канонический `render_lines`) в C++ не реализован — только
  чтение и байтовый round-trip.

## CLI

`gunmetal --dump SOBS/railrock.soe` печатает `thing`, `id`, `version`, число
полей/хвостовых токенов и размер карты (77). Ошибка парсера возвращает
структурированное сообщение, а не молчаливый fallback на токены.
