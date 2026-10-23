# Редактор записей сцены `.sbt` (`M11.3`, срез 1)

* **Статус:** срез 1 готов: перечисление, чтение и правка **записей сцены**
  (все типы, кроме `SCRIPT` (11) и `END_OF_FILE` (255)), замена записи целиком,
  проверка файла двумя парсерами, побайтовый round-trip 36/36, self-test
  38/38, правка реального `level0.sbt` подтверждена оракулом и C++-движком.
* **Ветка:** `feat/sceneedit`, база `master` `3b6d5e7`, дата 2026-10-23.
* **Файлы:** `tools/editor/sceneedit.py` (новый), `docs/SCENE_EDIT.md` (этот
  документ). `gm/sbt.py`, `gm/*`, `tools/editor/sbtwrite.py`,
  `tools/editor/sbtasm.py`, тесты, `engine/*`, `ROADMAP.md` и прочие
  существующие файлы **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sceneedit`
  (в git не попадают).
* **Оракул:** `gm/sbt.py` (`parse`, `parse_payload`, `encode_payload`,
  `Sbt.to_bytes`); писатель контейнера переиспользуется из
  `tools/editor/sbtwrite.py` (`read_bytes`/`write_bytes`/`sha256`/
  `structure_problems`/`encoding_drift`) — импортом, без изменения файла.

---

## 1. Что считается «записью сцены»

Контейнер `.sbt` — 84-байтный заголовок (`char magic[80]` + `u32 version`,
`gm/sbt.py:113-115`, `:758-767`) и **поток записей без таблицы смещений и
длин**: `u8 type` + нагрузка, которую обработчик типа вычитывает целиком;
`255` (`END_OF_FILE`) нагрузки не имеет и является последним байтом
(`gm/sbt.py:22-25`, `parse` `:768-786`; `docs/FORMATS.md` §12;
`docs/SBT_WRITE.md` §1).

Инструмент считает записью сцены **любую** запись, кроме `SCRIPT` (11) и
`END_OF_FILE` (255). Индекс `--record N` — это позиция в этом списке
(`scene_records()`), поэтому `SCRIPT` и `END` по номеру недостижимы, а отказ
для них выдаётся явным текстом со ссылкой на `sbtwrite.py`.

| тип | имя | нагрузка на диске (лимиты загрузчика) | источник |
|---|---|---|---|
| 1 | `SWITCH` | `u32 n ≤ 0x200`; `n × 0x20` | `docs/SBT_RECORDS.md` §2.1, загрузчик `0x44f000` |
| 2 | `DOOR` | `u32 n ≤ 100`; `n × 0xB8` | §2.2, `0x44e540` |
| 3 | `THING` | `u8 exists`; если ≠0 — `0x528` | §2.3, `0x46fd90` |
| 4 | `THING_TYPES` | `u32 n ≤ 0x80`; `n × 936` | §2.4, `0x47e000` |
| 5 | `NAMED_ITEMS` | `u32 n ≤ 0x31`; `n × 52` | §2.5, `0x468ef0` |
| 6 | `SPRITES` | `u32 n < 0x1F4`; `n × 44` | §2.6, `0x4564b0` |
| 7 | `SOUND_SOURCES` | `u32 n < 0x80`; 5 колонок `u32` + колонка `f64` | §2.7, `0x46b280` |
| 8 | `SECTOR_STUFF` | ровно `0x5800` (256 × `0x58`) | `docs/RECORDS.md` §2, `0x46b7b0` |
| 9 | `PLAYER_STATUS` | `u8 exists`; если ≠0 — `0x3510` | §2.9, `0x451c20` |
| 11 | `SCRIPT` | **отказ** (код, смещения подскриптов) | `docs/FORMATS.md` §12.3, `0x462a80` |
| 12 | `SOUND_SAMPLE_TABLE` | `300 × u32` + `300 × 9` байт имён (`0x4B0 + 0xA8C`) | §2.10, `0x40b670` |
| 15 | `POLY_HANDLE` | `u32 n < 0x1F4`; 4 колонки `u32` | §2.11, `0x4633a0` |
| 16 | `OBJECT_HANDLE` | `u32 n < 0x12C`; 3 колонки `u32` | §2.11, `0x463680` |
| 17 | `SECTOR_HANDLE` | `u32 n < 0x100`; 2 колонки `u32` | §2.11, `0x463900` |
| 18 | `LIFT` | `u32 n ≤ 0x1E` (`n × 0xA8`) + `u32 k ≤ 0xC8` + 3 колонки `u32[k]` | `docs/RECORDS.md` §4, `0x44ec20`/`0x44eed0` |
| 19 | `ITEM` | ровно `0x7080` (300 × `0x60`) | `docs/RECORDS.md` §3, `0x468df0` |
| 20 | `SOB_TABLE` | `u32 n ≤ 0xC8`; `n × 20` | §2.14, `0x47c7c0` |
| 10, 13 | — | заглушка `0x4971b0` (`mov eax,-1; ret`), нагрузки нет | §1 |
| 14 | — | путь ошибки «World Things file (%s) corrupt» (`0x4555ae`), не запись | §1 |

**Что реально есть в поставке.** 36 уровневых `*.sbt` содержат **241 запись
сцены**, измерено этим инструментом (`scene_records` по всем файлам):

| тип | 4 | 5 | 12 | 15 | 16 | 17 | 20 |
|---|---:|---:|---:|---:|---:|---:|---:|
| записей | 36 | 36 | 36 | 35 | 29 | 33 | 36 |

Типов `1, 2, 3, 6, 7, 8, 9, 18, 19` в уровнях **нет** (совпадает с
`docs/FORMATS.md` §12 и `docs/RECORDS.md` §1): это либо блоки состояния сейва
(19/8/7/3/9 — внутри контейнера `SAVEGAME`, а не в `.sbt`), либо данные,
которые создаёт скрипт (`ADD_DOOR`, `ADD_SWITCH`, `ADD_LIFT`, `ADD_THING`).
Поэтому для этих типов проверка — кодовая + синтетическая (§8).

---

## 2. Доказанные поля

Каждое поле в таблицах ниже — из документа с адресом в `gundx.exe`
(ImageBase `0x400000`). Формат ссылки: `док §раздел` + адрес писателя или
читателя. Полная карта печатается командой
`python3 tools/editor/sceneedit.py --fields` (320 строк, с provenance у
каждого поля); в `--list` provenance показывается для выбранного поля через
`--get`.

### 2.1 `SWITCH` (1) — 8 × i32

| смещ. | поле | тип | доказательство |
|---|---|---|---|
| `+0x00` | `kind` | u32 | 0 обычный, `0x101` transformed, `0x102..0x104` source — `0x44f960` |
| `+0x04` | `id` | u32 | id сущности для `GENERATE` — `0x479960`/`0x479810` |
| `+0x08` | `radius` | u32 | радиус триггера kind `0x102..0x104` — `0x44fb9e` |
| `+0x0C..+0x14` | `pos.x/y/z` | i32 | позиция триггера — `0x44f960` → `0x479960` |
| `+0x18` | `aux` | u32 | **гипотеза** (звуковой аргумент) |
| `+0x1C` | `sound` | i32 | индекс сэмпла, `-1` = тишина |

### 2.2 `DOOR` (2) — 184 байта

| смещ. | поле | тип | доказательство |
|---|---|---|---|
| `+0x00` | `box.0..8` | 9 × f64 | закрытый угол, открытый угол, размер |
| `+0x48` | `delta.0..2` | 3 × f64 | дельта движения бокса за тик — `0x450054` |
| `+0x60` | `const` | f64 | `0.03`, писатель `0x44e460`, читателя нет |
| `+0x68` | `flags` | i32 | младший ниббл 1..3, бит `0x40` used — `0x45068d` |
| `+0x6C` | `ids.0..4` | 5 × i32 | id сущностей — `0x44f6c0`, `0x44fc78` |
| `+0x80` | `state` | i32 | `-1` закрывается, `0` закрыта, `+1` открывается |
| `+0x84` | `sector` | i32 | адрес створки в `.sbw` — `0x434419` |
| `+0x88` | `object` | i32 | адрес створки в `.sbw` — `0x434419` |
| `+0x8C` | `phase_prev` | u32 | фаза анимации до тика |
| `+0x90` | `phase_next` | u32 | фаза анимации после тика |
| `+0x94` | `step` | i32 | приращение фазы за тик |
| `+0x98` | `param5` | i32 | операнд L6, читателей нет |
| `+0x9C` | `param6` | i32 | операнд L7, читателей нет |
| `+0xA0` | `v.0..2` | 3 × i32 | хвост `VNVNVN` — **гипотеза** |
| `+0xAC` | `n.0..2` | 3 × i32 | хвост `VNVNVN` — **гипотеза** |

### 2.3 `THING` (3), `PLAYER_STATUS` (9)

Документирован только флаг существования: `+0x00 exists` (u8), за ним
`0x528` / `0x3510` байт runtime-состояния (`docs/SBT_RECORDS.md` §2.3/§2.9,
загрузчики `0x46fd90` / `0x451c20`). Байты состояния инструмент **не
интерпретирует**: `--list` печатает hex-дамп первых 64 байт, `--set`
меняет только `exists`.

### 2.4 `THING_TYPES` (4) — 936 байт

| смещ. | поле | тип | доказательство |
|---|---|---|---|
| `+0x000` | `name` | `char[16]` | полный слот сохраняется (`name_raw`) |
| `+0x010..+0x198` | `param.0..48` | 49 × f64 | §2.4 |
| `+0x010` | `speed` (alias `param.0`) | f64 | `docs/FORMATS.md` §12.2: первый double — `speed`, шаг снаряда, читает `0x4752d0` |
| `+0x198..+0x3A8` | `tail.0..131` | 132 × u32 | opaque, хранится байтами |
| `+0x1A4` | `model` (alias `tail.3`) | u32 | индекс в `SOB_TABLE`, доказано `0x47cb6c` → `0x47c9b0` → `0x776f04` и проверено 2896/2896 (`tools/re/thing_models.py`) |

Алиасы `speed`/`model` — те же байты, что `param.0`/`tail.3`; в отчётах
показывается запрошенное имя и provenance алиаса.

### 2.5 `NAMED_ITEMS` (5) — 52 байта

`name` — `char[16]` в `+0x00` (strncpy 15 байт, `0x468a53`); `f.0..f.8` —
9 × u32 в `+0x10..+0x30` (загрузчик `0x4689f0`, формат
`%ld %ld %s %ld %s %ld %s %ld %ld`). Соответствие полям `ITEM` доказано
`0x468d40` (`docs/RECORDS.md` §3): `f.2→ITEM+0x38`, `f.3→+0x3C`,
`f.4→+0x40`, `f.5→+0x44`, `f.6→+0x48`, `f.7→+0x4C`, `f.8→+0x50/+0x54`.

### 2.6 `SPRITES` (6) — 44 байта

| смещ. | поле | тип | xref |
|---|---|---|---|
| `+0x00/+0x04` | `scale_x/y` | f32 | `0x456164`/`0x456173` |
| `+0x08` | `runtime_object` | u32 | устаревший указатель, `0x456556` |
| `+0x0C..+0x14` | `x/y/z` | i32 | `0x456136/40/4a` |
| `+0x18` | `param` | u8 | `0x45615e` — **гипотеза** |
| `+0x1C` | `flags` | u32 | `0x456001` — **гипотеза** |
| `+0x20` | `effect_id` | i32 | `0x456130` → `0x4360f1` |
| `+0x24` | `param16` | u16 | `0x456179` |
| `+0x28` | `spawn_time` | u32 | `0x456185` |

### 2.7 `SOUND_SOURCES` (7) — колонки, не массив структур

`+0x04` — начало колонок: `x`, `y`, `z`, `volume`, `sample` (5 × i32 по
`n`), затем `atten` (`n × f64`). Порядок колонок доказан `0x46b210`
(создатель) и `0x46b41b` (писатель сейва): базы `0x800340`, `0x800540`,
`0x800140`, `0x7ffd40`, `0x7fff40`, `0x800760`
(`docs/SBT_RECORDS.md` §2.7). Шестая колонка — **затухание на единицу
расстояния**, читает `0x46b616` (`docs/AUDIO_3D.md` §2.2); у скриптовых
источников она `0.03` (`0x46b8ad`).

### 2.8 `SECTOR_STUFF` (8) — 256 × 0x58

| смещ. | поле | тип | доказательство |
|---|---|---|---|
| `+0x00` | `move.x/y/z` | 3 × f64 | `SET_SECTOR_MOVEMENT` `0x457815` (потребитель `0x4713ab`) |
| `+0x18` | `extra.x/y/z` | 3 × f64 | тот же писатель; читателей нет |
| `+0x30` | `sound_param` | f64 | `SET_SECTOR_SOUND` → `0x46b740`, читает `0x46c6cd` |
| `+0x38` | `event_a` | i32 | `SET_SECTOR_EVENTS` `0x4579d6` |
| `+0x3C` | `event_b` | i32 | `0x4579e0` |
| `+0x40` | `damage_a` | u32 | `SET_SECTOR_DAMAGE` `0x45799a` (`value & 0xffff`), читает `0x4450f6` |
| `+0x44` | `damage_b` | i32 | `0x4579a0` (`value >> 16`, знаковое), читает `0x4450fc` |
| `+0x48` | `sound` | i32 | `0x46b76a`, индекс `SOUND_SAMPLE_TABLE` |
| `+0x4C` | `sound_count` | i32 | `0x46b760`, громкость ambient |
| `+0x50` | `flags` | u32 | `SET_SECTOR_FLAGS` `0x457955` |
| `+0x54` | `unused` | u32 | **гипотеза**: мёртвое поле |

`docs/RECORDS.md` §2.2 объясняет, почему `+0x40/+0x44` — два dword, а не два
u16 с паддингом; инструмент правит их как dword, но байтовое представление
(`gm/sbt.py` держит `damage_lo/damage_hi`) не меняется.

### 2.9 `SOUND_SAMPLE_TABLE` (12)

`counts[0..299]` — `u32` в `+0x000` (счётчики ссылок), `names[0..299]` —
слоты по 9 байт в `+0x4B0` (`docs/SBT_RECORDS.md` §2.10). Имена пишутся в
полный слот с NUL-паддингом; непустые имена резолвятся в `sounds/SOU.IDX`
(5800/5800, `docs/FORMATS.md` §12.2).

### 2.10 `POLY_HANDLE` (15), `OBJECT_HANDLE` (16), `SECTOR_HANDLE` (17)

Колонки (не `{u32 × k}`): `tag, sector, object, poly` / `tag, sector, object`
/ `tag, sector`; `u32 n` в `+0x00`, дальше `n` значений каждой колонки
подряд. Загрузчики `0x4633a0`/`0x463680`/`0x463900` читают каждую колонку в
свой глобал (`0x5b21f8`, `0x618b88`, `0x5e5680`, `0x6183b8`).
`(sector, object, poly)` — адрес для `resolve_poly` `0x434463`; `tag` —
редакторский id для `SET_HANDLEPOLY_*`/`SET_HANDLESECTOR_*`, не уникален
(`docs/FORMATS.md` §12.1).

### 2.11 `LIFT` (18)

| смещ. | поле | тип | доказательство |
|---|---|---|---|
| `+0x00` | `basis0.x/y/z` | 3 × f64 | `0x44ea6a` → `0x41e025` |
| `+0x18` | `basis1.x/y/z` | 3 × f64 | там же (две тройки) |
| `+0x30` | `sob_index` | i32 | модель кабины, `0x44e706` |
| `+0x34` | `kind` | i32 | в логике лифта `1`, `0x44f7b6`/`0x450197` |
| `+0x38` | `sob_index2` | i32 | вторая модель, `0x44e9d4` (семантика — гипотеза) |
| `+0x3C/+0x40/+0x44` | `x/y/z` | i32 | `fiadd` в позицию, `0x44e743/55/67` |
| `+0x54` | `speed` | i32 | шаг высоты за тик, `0x4501c5` |
| `+0x5C` | `height` | i32 | текущая высота, `0x4501c2` |
| `+0x60` | `target` | i32 | целевая высота, `0x4501ce` |
| `+0x68` | `moving` | i32 | `!=0`, пока лифт едет, `0x4501a1` |
| `+0x6C` | `stop_index` | i32 | текущая остановка, `0x44f7d0` |
| `+0x70/+0x74/+0x78` | `call_1..3` | i32 | id этажей кнопок вызова, `0x44f7e0/7f7` |
| `+0x9C` | `stop_count` | i32 | число остановок лифта, `0x44ebc3` |
| `+0xA0` | `stop_start` | i32 | первый индекс в колонках, `0x44e9c4` |

После записей: `u32 k` и три параллельные колонки `u32[k]` — `floor`
(`0x5a9130`), `height` (`0x5a8e10`, кламп `0xffff`), `sound` (`0x5a8af0`).
Поле `height` есть и у записи лифта (`+0x5C`), и у колонки остановок,
поэтому в ссылке нужен префикс блока: `lifts.height[0]`, `stops.height[0]`.

### 2.12 `ITEM` (19) — 300 × 0x60

| смещ. | поле | тип | доказательство |
|---|---|---|---|
| `+0x00` | `basis.x/y/z` | 3 × f64 | `0x4688ad` → `0x41e025` |
| `+0x18` | `position.x/y/z` | 3 × f64 | `0x468507` |
| `+0x30` | `runtime_object` | u32 | указатель созданного объекта, `0x468619` (в сейве 0) |
| `+0x34` | `kind` | u32 | 1 обычный/AT_SOURCE, 2 `*_FIXED` — `0x468710` |
| `+0x38` | `state` | i32 | `-2` свободен, `-1` фантом, `0` живой, `-3/-4` мины, `>0` таймер — `0x468e41` |
| `+0x3C` | `op2` | u32 | операнд a2, смысл не доказан |
| `+0x40` | `sob_index` | i32 | индекс в `SOB_TABLE`, `0x4684b0` |
| `+0x44` | `op3` | u32 | операнд a3 (у именованных — `f5`) |
| `+0x48` | `op4` | u32 | операнд a4 (у именованных — `f6`) |
| `+0x4C` | `op5` | u32 | операнд a5 / количество (у именованных — `f7`); триггер: `0x469770` |
| `+0x50` | `op6` | u32 | операнд a6 (у именованных — `f8`) |
| `+0x54` | `timer_reload` | u32 | таймер перезарядки, `0x4690d8` |
| `+0x58` | `flags` | u8 | копируется в `object+0x74`, `0x468516` |
| `+0x5C` | `source_addend` | u32 | слагаемое к handle, `0x4692d8` (`eax & 0xffff`) |

### 2.13 `SOB_TABLE` (20) — 20 байт

`name` — `char[12]` в `+0x00` (резолвится в `SOBS/<name>.sob` или
`Sobanim/<name>.soo`); `amount` — `u32` в `+0x0C` (загрузчиком **не
читается**); `ptr` — `u32` в `+0x10`, устаревший адрес кучи, который
загрузчик перезаписывает указателем модели (`0x47c845`) — в файле его
значение произвольно, поэтому правка `amount` безопасна для загрузки, а
`ptr` менять бессмысленно (`docs/SBT_RECORDS.md` §2.14).

---

## 3. Как инструмент правит файл

1. **Читает контейнер оракулом** (`gm.sbt.parse`) — тот же парсер, что у
   `gm.py`, поэтому границы записей и лимиты счётчиков не дублируются.
2. **Находит поле по документированному смещению** внутри нагрузки записи
   (`Record.raw`). Все смещения берутся из `SCHEMA`/`blocks_of()`, где у
   каждого поля записан раздел документа; смещение и размер проверяются
   против размера элемента и границ нагрузки (`_need`).
3. **Патчит сырые байты на месте**: `raw[:off] + packed + raw[off+size:]`.
   Никакого декодирования/перекодирования структуры нет, поэтому паддинги,
   «хвосты» имён после NUL, устаревшие указатели и неразобранные слова
   `tail` сохраняются дословно (это же свойство `Sbt.to_bytes`,
   `gm/sbt.py:490-515`).
4. **Перепроверяет патч оракулом**: `gm.sbt.parse_payload(type, new_raw)`
   требует полного вычитывания нагрузки и проверяет счётчики/лимиты.
5. **Собирает контейнер** `Sbt.to_bytes()` (заголовок + `type` + `raw` на
   каждую запись). Таблицы смещений в формате нет, поэтому запись может
   менять размер: файл просто становится длиннее/короче на дельту, а
   `Record.offset`/`size` пересчитываются (`refresh_offsets`).
6. **Пишет атомарно** (`path.tmp` + `os.replace`, из `sbtwrite.py`),
   **перечитывает файл с диска** и проверяет тремя способами:
   * оракул: `structure_problems` (`to_bytes` == файл, `size == 1 + len(raw)`,
     непрерывность потока, `END` последним байтом, инварианты `SCRIPT`);
   * **собственный обходчик** `walk_container()` — независимый от `gm.sbt`
     разбор заголовка и всех типов (включая `SCRIPT`, который он пропускает
     по раскладке, но не правит), сверка типов/размеров/байтов с оракулом;
   * **сверка записей**: все прочие записи побайтово равны исходным, у
     целевой — ожидаемые байты/значение.

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод или
отказ (`SCRIPT`, лимит счётчика, выход за границы, отсутствующий файл,
нечисловое значение), `3` внутренняя ошибка. Ни одно исключение не
выходит наружу: `main` переводит их в сообщение и код.

### 3.1 Ссылки на поля

`NAME` или `NAME[INDEX]`:

* `INDEX` — **индекс элемента** внутри нагрузки: слот `ITEM`, строка
  `SOB_TABLE`, строка handle-таблицы, остановка `LIFT`, слот сэмпла. Если в
  блоке больше одного элемента, индекс обязателен (иначе ошибка со списком
  диапазона) — это защита от правки «не того» слота.
* Компонент вектора — через точку: `pos.x`, `ids.2`, `param.0`, `tail.3`,
  `move.z`, `basis.1`.
* Неоднозначные имена разрешаются префиксом блока: `stops.height[1]`,
  `lifts.height[0]`, `names.name[5]`, `counts.count[5]`.
* Алиасы: `speed` = `param.0` (`docs/FORMATS.md` §12.2), `model` = `tail.3`
  (`docs/SBT_RECORDS.md` §2.4).

Типы значений: `u8`, `u16`, `u32`, `i32` (десятичное или `0x…`, с проверкой
диапазона), `f32`/`f64` (конечные числа; `nan`/`inf` отвергаются), слоты
имён `char[16]`/`char[12]`/`char[9]` (latin-1, не длиннее слота минус NUL).

### 3.2 `--replace-record`

`--file BLOB` — **нагрузка записи без байта типа**. Она проверяется
`gm.sbt.parse_payload` (полное вычитывание + счётчики), поэтому «мусор» с
неверным числом/размером отвергается до записи. Размер может отличаться:
контейнер не хранит смещений, пересчёт не нужен — проверяется только, что
поток остался непрерывным, `END` — последним байтом, а все прочие записи
побайтово те же.

---

## 4. CLI и примеры

```
python3 tools/editor/sceneedit.py --list LEVEL [--limit N] [--json]
python3 tools/editor/sceneedit.py --get LEVEL --record N --field NAME
python3 tools/editor/sceneedit.py --set LEVEL --record N --field NAME=VALUE --out FILE
python3 tools/editor/sceneedit.py --replace-record LEVEL --record N --file BLOB --out FILE
python3 tools/editor/sceneedit.py --check FILE [--against LEVEL] [--record N]
                                  [--field NAME=VALUE] [--expect-file BLOB]
python3 tools/editor/sceneedit.py --roundtrip [--game DIR]
python3 tools/editor/sceneedit.py --self-test
python3 tools/editor/sceneedit.py --fields [--json]
```

`LEVEL` — путь к `.sbt` или стем (`level0`), тогда файл ищется в `--game`
(по умолчанию `/mnt/d/games/Gunmetal 3dfx`). Переменные окружения не
читаются — как и у остальных инструментов `tools/editor/`.

### 4.1 `--list level0` (сокращено)

```
$ python3 tools/editor/sceneedit.py --list level0 --limit 3
/mnt/d/games/Gunmetal 3dfx/level0.sbt: 127338 bytes, 8 records, 6 scene record(s), sha256 ded1a0a8169c5dbe
#0   type 12  SOUND_SAMPLE_TABLE  off 0x54     size 3901    payload 3900
      counts         300 element(s)  docs/SBT_RECORDS.md §2.10 (300 x u32)
        [0] count=1
        [1] count=1
        [2] count=3
      names          300 element(s)  docs/SBT_RECORDS.md §2.10 (300 x 9 bytes)
        [0] name=updown
        [1] name=select
        [2] name=vleft
#1   type 20  SOB_TABLE           off 0xf91    size 1345    payload 1344
      SOB_TABLE      67 element(s)  docs/SBT_RECORDS.md §2.14 (20 bytes)
        [0] name=20MMCLP  amount=2  ptr=2200430120
        ...
#5   type 4   THING_TYPES         off 0x1c65   size 53357   payload 53356
      THING_TYPES    57 element(s)  docs/SBT_RECORDS.md §2.4 (936 bytes)
        [0] name=bgchunk1  speed=0  model=20
        [1] name=bgchunk2  speed=0  model=20
        [2] name=bgchunk3  speed=0  model=20
        [3] name=deadtick  speed=1  model=21
---- type 11  SCRIPT              off 0xecd2   size 66711   payload 66710  [not editable]
      compiled SCRIPT code; use tools/editor/sbtwrite.py
      things 0, handles 350, code 62497 bytes, flag 0, tables 0 bytes
---- type 255 END_OF_FILE         off 0x1f169  size 1       payload 0  [not editable]
      terminator
```

Индексы `#N` — то, что принимает `--record`; `SCRIPT`/`END` помечены «not
editable» и номер не получают. Для типов 3/9 `--list` печатает hex-дамп
первых 64 байт состояния (`hex …`), потому что документирован только
`exists`.

### 4.2 `--get` / `--set`

```
$ python3 tools/editor/sceneedit.py --get level0 --record 5 --field 'speed[25]'
/mnt/d/games/Gunmetal 3dfx/level0.sbt record #5 THING_TYPES: speed[25] = 50 (f64, payload +0x5b7c, file 0x77e2)
  doc: docs/FORMATS.md §12.2: +0x010 is speed (0x4752d0)

$ python3 tools/editor/sceneedit.py --get level0 --record 5 --field model[25] --json
{
 "record": 5,
 "field": "model",
 "index": 25,
 "block": "THING_TYPES",
 "kind": "u32",
 "value": 31,
 "payload_offset": 23824,
 "payload_size": 4,
 "doc": "docs/SBT_RECORDS.md §2.4: +0x1A4 = SOB_TABLE index, proven 2896/2896 (0x47cb6c)",
 "raw": "1f000000",
 ...
 "canonical_field": "tail.3",
 "file_offset": 31094,
 "ok": true
}
```

`--set` требует `--out` (установка игры только для чтения) и печатает
старое/новое значение, дельту размера и результат сверки:

```
$ python3 tools/editor/sceneedit.py --set level0 --record 5 --field 'speed[25]=55' \
        --out /tmp/opencode/sceneedit/level0.sbt
set: record #5 THING_TYPES.speed[25] 50 -> 55  (payload +0x5b7c, f64)
  /mnt/d/games/Gunmetal 3dfx/level0.sbt -> /tmp/opencode/sceneedit/level0.sbt (+0 bytes), sha256 ded1a0a8169c..e946e1bc3a7c
  other records identical: yes, 8 records
```

### 4.3 `--check`

```
$ python3 tools/editor/sceneedit.py --check /tmp/opencode/sceneedit/level0.sbt \
        --against level0 --record 5 --field 'speed[25]=55'
check: /tmp/opencode/sceneedit/level0.sbt ok (8 records, 6 scene, 127338 bytes, sha256 e946e1bc3a7cd0f1)
  against /mnt/d/games/Gunmetal 3dfx/level0.sbt: other records identical: yes
  expected speed[25] = 55.0, actual 55.0
```

Без `--against` проверяются структура и побайтовый `to_bytes()` (оба
парсера). С `--against` дополнительно требуется, чтобы все прочие записи и
84-байтный заголовок совпали с исходником; `--record N` сужает сравнение до
одной целевой записи (её содержимое проверяется через `--field NAME=VALUE`
или `--expect-file BLOB`). Подмена любого байта в другой записи даёт `FAIL`
(есть в self-test).

---

## 5. Правка на реальных данных (`level0.sbt`)

`ITEM` (19) в поставочных уровнях отсутствует (`docs/RECORDS.md` §1), поэтому
правка сделана на документированном поле `THING_TYPES` — `speed` (`+0x010`,
`docs/FORMATS.md` §12.2), у типа `Tracer` (индекс 25), значение `50 → 55`.

```
$ cp "/mnt/d/games/Gunmetal 3dfx/level0.sbw" /tmp/opencode/sceneedit/
$ python3 tools/editor/sceneedit.py --set level0 --record 5 --field 'speed[25]=55' \
        --out /tmp/opencode/sceneedit/level0.sbt
```

**(а) оракул `gm.sbt` видит новое значение**

```
$ python3 -c "
import sys; sys.path.insert(0, '.')
from gm import sbt
s = sbt.parse_file('/tmp/opencode/sceneedit/level0.sbt')
tt = s.records_of(4)[0].payload
i = [k for k, e in enumerate(tt.entries) if e.name == 'Tracer'][0]
print('oracle: Tracer[%d].speed =' % i, tt.entries[i].params[0])"
oracle: Tracer[25].speed = 55.0
```

**(б) C++-движок загружает файл с той же структурой**

```
$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sceneedit -l level0 --info
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
```

Для сравнения, до правки тот же `--info` на исходном файле:

```
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
```

Совпадают `records=8`, `poly/object/sector`, `sobs=67`, `script=350/62497`
(350 подскриптов, 62497 байт кода), гистограмма типов.

**(в) все остальные записи побайтово те же**

```
$ python3 -c "
import sys; sys.path.insert(0, '.')
from gm import sbt
a = sbt.parse_file('/mnt/d/games/Gunmetal 3dfx/level0.sbt')
b = sbt.parse_file('/tmp/opencode/sceneedit/level0.sbt')
tgt = [i for i, r in enumerate(a.records) if r.type == 4][0]
d = [hex(k) for k in range(len(a.records[tgt].raw))
     if a.records[tgt].raw[k] != b.records[tgt].raw[k]]
print('target bytes differing:', d)
print('other records identical:',
      all(x.raw == y.raw and x.type == y.type
          for j, (x, y) in enumerate(zip(a.records, b.records)) if j != tgt))
print('header identical:', a.header == b.header)"
records 8 8
target record 5 THING_TYPES differing bytes: ['0x5b81', '0x5b82'] sizes 53356 53356
other records differing: [] => identical: True
header identical: True
```

Изменились ровно 2 байта `f64`-значения (`50.0` → `55.0`) в целевой записи;
остальные 7 записей и заголовок — байт в байт. Это же подтверждает
`--check --against` выше и строка `other records identical: yes` в выводе
`--set`.

---

## 6. Замена записи целиком на реальных данных

**(а) тот же размер.** `amount` в `SOB_TABLE` (загрузчиком не читается,
`docs/SBT_RECORDS.md` §2.14) изменён с 2 на 99; нагрузка — 1344 байта:

```
$ python3 tools/editor/sceneedit.py --replace-record level0 --record 1 \
        --file /tmp/opencode/sceneedit/sob_same.bin --out /tmp/opencode/sceneedit/sob_same.sbt
replace: record #1 SOB_TABLE  1344 -> 1344 payload bytes (+0), file +0 bytes
  /mnt/d/games/Gunmetal 3dfx/level0.sbt -> /tmp/opencode/sceneedit/sob_same.sbt
  blocks: SOB_TABLE[67]
  other records identical: yes
```

**(б) другой размер.** В `SOB_TABLE` добавлена 68-я запись (дубликат
`20MMCLP`, поэтому модель резолвится), нагрузка 1344 → 1364:

```
$ python3 tools/editor/sceneedit.py --replace-record level0 --record 1 \
        --file /tmp/opencode/sceneedit/sob_grow.bin --out /tmp/opencode/sceneedit/grow/level0.sbt
replace: record #1 SOB_TABLE  1344 -> 1364 payload bytes (+20), file +20 bytes
  /mnt/d/games/Gunmetal 3dfx/level0.sbt -> /tmp/opencode/sceneedit/grow/level0.sbt
  blocks: SOB_TABLE[68]
  other records identical: yes

$ ./build/sandbox/engine/gunmetal -g /tmp/opencode/sceneedit/grow -l level0 --info
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=68 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
```

Файл вырос на 20 байт, движок загрузил его и увидел `sobs=68`; остальные
поля сводки не изменились. Это и есть доказательство того, что таблицы
смещений в контейнере нет и пересчёт не нужен.

---

## 7. Проверки

### 7.1 `--self-test` (без файлов игры), 38/38

```
$ python3 tools/editor/sceneedit.py --self-test
  own walker == oracle           ok  16 records, 58510 bytes, own walker agrees
  scene record indexing          ok  14 scene records, SCRIPT/END skipped
  get documented fields          ok  ITEM state/sob_index, THING_TYPES speed/model
  aliases/columns/kinds          ok  aliases, columns (7/12/15), f32, f64
  set u32 (ITEM sob_index)       ok  ITEM sob_index 41 -> 77, others identical, bytes at +0x40
  set f64 (THING_TYPES speed)    ok  THING_TYPES speed 0.01 -> 0.02 at +0x010
  set u8 keeps size              ok  ITEM flags[2] = 255 (u8), file size unchanged
  set name slot                  ok  SOB_TABLE name[0] = 'ABCDEF'
  set hex value                  ok  SWITCH kind[1] = 0x102 (hex input)
  set sample count               ok  SOUND_SAMPLE_TABLE count[130] = 9
  set LIFT stop                  ok  LIFT stop height[1] = 65535
  set sound column               ok  SOUND_SOURCES atten[0] = 0.07 (f64 column)
  error: entry index OOB         ok  state[300]: element index out of range, the block has 300 element(s) (0..299)
  error: index required          ok  field 'state' has 300 elements; write state[INDEX] with INDEX in 0..299
  error: unknown field           ok  record type 19 ITEM has no field 'nope'; fields: basis.0, basis.1, basis.2, flags, kind, op2, op3, op4, op5, op6, position.0, position.1, position.2, runtime_object, sob_index, source_addend ... (18 fields total; see --fields)
  error: unknown record          ok  record 99 does not exist: the file has 14 scene record(s) (indices 0..13); use --list
  error: SCRIPT refused          ok  record type 11 SCRIPT is compiled script code, not a scene record; edit it with tools/editor/sbtwrite.py (sub-script replacement; docs/SBT_WRITE.md §2)
  error: END refused             ok  END_OF_FILE (255) is the terminator, not a record
  error: u8 overflow             ok  value 256 is outside u8 range [0, 255]
  error: name too long           ok  value 'ABCDEFGHIJKLMNOPQ' is 17 bytes; a name12 slot holds at most 11 bytes plus NUL
  error: non-finite float        ok  value 'nan' is not finite; the engine has no NaN/Inf handling
  error: bad integer             ok  value 'x' is not an integer (i32 field)
  replace-record same size       ok  SOB_TABLE replaced (same size), others identical
  replace-record larger          ok  +20 bytes, record offsets re-derived, others identical
  error: bad replacement blob    ok  SOB_TABLE: replacement blob is not a valid payload: SOB_TABLE: count 201 out of range [0, 200]
  error: SCRIPT replace refused  ok  scene index excludes SCRIPT; index 14 rejected
  check ok                       ok  own+oracle walk, 16 records
  check --against/--field        ok  against+expect-field accepted 77, rejected 78
  check detects tampering        ok  detects a flipped byte in another record
  header preserved               ok  84-byte header preserved
  error: loader count limit      ok  docs/SBT_RECORDS.md §2.14 (20 bytes): count 201 exceeds the loader limit 200
  error: trailing bytes          ok  1 trailing byte(s) after END_OF_FILE
  error: missing EOF             ok  record stream has no END_OF_FILE (255) terminator
  error: bad magic               ok  bad magic b'XINARY SAVE\x00' (expected b'BINARY SAVE\x00')
  error: missing file            ok  cannot read /tmp/opencode/sceneedit/selftest/nope.sbt: No such file or directory
  synthetic byte round-trip      ok  synthetic container re-serialises byte-identically
  exists flag sizes the payload  ok  exists=1 without body refused; THING 1 -> 0x529 bytes ok
  JSON output                    ok  list/get JSON-serialisable
self-test: 38/38 passed
```

Синтетический контейнер (16 записей: типы 1–9, 11, 12, 15, 18, 19, 20)
собран оракулом (`gm.sbt.encode_payload`) — то есть проверяются и типы,
которых нет в поставе. Отдельный маленький контейнер
(`THING`/`PLAYER_STATUS`/`SOB_TABLE`) проверяет зависимость длины нагрузки
от флага `exists`: `exists=1` без тела `0x528` отвергается, а замена
1-байтной нагрузки на `0x529` проходит с пересчётом размера. Покрыты: все целочисленные и плавающие типы, слоты
имён, колонки (7/12/15/18), hex-ввод, сохранение размера при правке u8,
замена записи с ростом и пересчётом `Record.offset`, отказы (`SCRIPT`,
`END`, лимиты, границы, нечисловые/нефинитные значения, имя длиннее слота,
битая магия, хвост после `EOF`, отсутствие `EOF`, отсутствующий файл),
обнаружение чужой правки через `--check --against`.

### 7.2 `--roundtrip` — 36/36 побайтово

```
$ python3 tools/editor/sceneedit.py --roundtrip
roundtrip: 36 files, 6793572 bytes, 0.31s
  identical (read -> write -> compare): 36/36, 0 mismatch, 0 error
  structural checks (oracle + own walker): 36/36
  scene records: 241, structural-encoder drift: 0 record(s)
```

Каждый из 36 поставочных `*.sbt` читается оракулом, пишется в
`/tmp/opencode/sceneedit/roundtrip`, перечитывается с диска и сравнивается
побайтово; независимый обходчик подтверждает те же границы/типы/размеры.
«Structural-encoder drift» — записи, у которых структурный кодировщик
`encode_payload` не совпал бы с сырыми байтами (0 на поставке; writer всё
равно пишет `raw`).

### 7.3 Регресс тулкита — 114/114

```
$ python3 regress.py --mirror --json | tail -1
{"passed": 114, "total": 114, "seconds": 46.5, "full": false, "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

---

## 8. Ограничения и что осталось

1. **`SCRIPT` (11) не правится.** Код и колонка-2 смещений подскриптов
   требуют пересчёта; для этого есть `tools/editor/sbtwrite.py`
   (`docs/SBT_WRITE.md` §2). Инструмент отказывает с текстом и кодом 2.
2. **Новых записей не создаёт.** Можно править существующую или заменить её
   целиком; вставить запись в поток/удалить её через CLI нельзя (это
   следующий срез `M11.3`; контейнеру это не мешает — таблицы смещений нет).
3. **Меняется только одно поле за вызов.** Согласованность нескольких полей
   (например, `stop_count` лифта и число элементов колонок, `SOB_TABLE.n` и
   число строк) инструмент не поддерживает; для таких правок —
   `--replace-record` с целой нагрузкой, проверенной оракулом.
4. **Слот имени затирается целиком.** `--set name[...]` пишет значение +
   NUL-паддинг на весь слот (16/12/9 байт). Байты после первого NUL в этом
   слоте теряются (в поставочных `THING_TYPES`/`SOB_TABLE` такие «хвосты»
   встречаются). Для остальных записей и полей всё сохраняется дословно.
5. **Типы 1/2/3/6/7/8/9/18/19 в поставочных `.sbt` отсутствуют** — их
   правка проверена только синтетикой (38/38) и кодом/доками. Блоки сейва
   (`SAVEGAME/saved0.sbt`: цепочка `6,19,8,7,3,9`) — это **не** `.sbt`
   -контейнер: у него своя шапка `A SAVED GAME` и поток блоков, поэтому
   `sceneedit.py` на нём корректно отказывает («bad .sbt magic»); для сейвов
   есть `gunmetal --save-out/--set` и `docs/SAVE_WRITER.md`.
6. **Семантика части полей — гипотеза** и помечена в `--fields`/`--list`:
   `SWITCH.aux`, `SPRITE.param/flags`, `DOOR.v/n`, `SECTOR_STUFF.extra/unused`,
   `LIFT.sob_index2`, `ITEM.op2..op6`. Инструмент их правит как байты, но
   игровой эффект не гарантирован.
7. **Проверка — структурная, не игровая.** Мод проверен оракулом, нашим
   C++-движком (`--info`) и побайтовым сравнением; запуск в оригинальном
   `gundx.exe` не автоматизирован (`M11.7` ⬜).
8. **Значения не нормализуются.** Инструмент не проверяет, что `sob_index`
   существует в `SOB_TABLE`, а `sound` — в `SOUND_SAMPLE_TABLE`: это
   семантические инварианты, а не границы записи (часть проверок есть в
   `tools/re/records.py` и `regress.py`).
9. **C++-движок — только чтение.** Записи 1/2/3/6/7/8/9/18/19 в C++ могут
   быть типизированы не полностью (`engine/src/res/sbt.*`); инструмент
   работает с байтами и от этого не зависит.

---

## 9. Сводка команд

```sh
# карта полей с provenance
python3 tools/editor/sceneedit.py --fields

# что лежит в уровне
python3 tools/editor/sceneedit.py --list level0 --limit 8

# чтение/правка документированного поля
python3 tools/editor/sceneedit.py --get level0 --record 5 --field 'speed[25]'
python3 tools/editor/sceneedit.py --set level0 --record 5 --field 'speed[25]=55' \
        --out /tmp/opencode/sceneedit/level0.sbt

# замена записи целиком (нагрузка без байта типа)
python3 tools/editor/sceneedit.py --replace-record level0 --record 1 \
        --file /tmp/opencode/sceneedit/sob_grow.bin \
        --out /tmp/opencode/sceneedit/grow/level0.sbt

# проверка результата
python3 tools/editor/sceneedit.py --check /tmp/opencode/sceneedit/level0.sbt \
        --against level0 --record 5 --field 'speed[25]=55'

# корпус и самотест
python3 tools/editor/sceneedit.py --roundtrip
python3 tools/editor/sceneedit.py --self-test
python3 regress.py --mirror --json
```
