# C++-читатель сейвов Gunmetal

* **Статус:** чтение контейнера, цепочки блоков и блока игрока (9) с паритетом
  Python на `saved0.sbt`/`saved1.sbt`. Запись не реализована.
* **Ветка:** `feat/cpp-save`, база `master` `eb6ccac`, дата 2026-10-20.
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx/SAVEGAME`).
* **Файлы:** `engine/src/res/save.h`, `engine/src/res/save.cpp`,
  `engine/tests/test_save.cpp`, регистрация теста в `engine/tests/CMakeLists.txt`.
* **Эталон:** `tools/re/savefile.py`, `tools/re/save_fields.py`,
  `docs/SAVE.md`, `docs/SAVE_FIELDS.md`.

Реализация не импортирует Python и не зависит от `gm.py`: все размеры блоков
берутся из counts, как в `save_rebuild.py`/`save_fields.py`.

## 1. Контейнер

```
0x000  u8   name[0x50]     заголовок сейва: первые 12 байт ("A SAVED GAME" / "2")
0x050  u32  reserved       0 в обоих поставочных сейвах
0x054  u8   level[0x50]    имя уровня, NUL-terminated ("LEVEL0")
0x0a4  u32  F[22]          поля F0..F21 (docs/SAVE_FIELDS.md §2)
0x0fc  4 x char[16]        "K-9", "", "coargus", "none"
0x13c  поток [u8 id][payload] ... [0xff]
```

Выравнивания, контрольной суммы и хвоста после `0xff` нет; последний байт
файла — `0xff`. Размер блока выводится из count, а не из смещений. Блоки
1/2/6/7/18 при нулевом count в файле отсутствуют; блоки 16/18 в `LEVEL0` не
встречаются (16 отсутствует, 18 имеет count 0), но парсер их поддерживает.

`SaveHeader::version` — сырой dword `+0x0c`. В `saved0` там `0x34`, в `saved1`
— `0`. Это **не версия контейнера**, а хвост 12-байтового заголовка
(`docs/SAVE.md` §7); поле оставлено для паритета с `savefile.py`, который
печатает его как «version».

Таблица блоков `saved0`/`saved1` (у обоих сейвов одинаковые id, смещения и
размеры; `count` — значение, которое возвращает `SaveBlock::count`):

| id | блок | offset | size | count | как разобран |
|---|---|---|---|---|---|
| 12 | sound table | `0x13c` | 3901 | 175 | 300 counts + 300×9 имён, count = ненулевые counts |
| 20 | SOB table | `0x1079` | 1345 | 67 | `u32 n + n×20` |
| 15 | poly handles | `0x15ba` | 1669 | 104 | `u32 n + n×16` |
| 17 | sector handles | `0x1c3f` | 213 | 26 | `u32 n + n×8` |
| 5 | named items | `0x1d14` | 57 | 1 | `u32 n + n×52` |
| 4 | thing types | `0x1d4d` | 53357 | 57 | `u32 n + n×936` |
| 11 | script | `0xedba` | 76345 | 10 | runtime-форма: things/named/code/tables |
| 2 | doors | `0x217f3` | 4605 | 25 | `u32 n + n×0xB8` |
| 1 | switches | `0x229f0` | 773 | 24 | `u32 n + n×0x20` |
| 6 | sprites | `0x22cf5` | 181 | 4 | `u32 n + n×0x2c` |
| 19 | ITEM | `0x22daa` | 28801 | 0 | ровно `0x7080`, тело не читается |
| 8 | SECTOR_STUFF | `0x29e2b` | 22529 | 0 | ровно `0x5800`, тело не читается |
| 7 | sound sources | `0x2f62c` | 173 | 6 | `u32 n + n×28` |
| 3 | things | `0x2f6d9` | 8181 | 6 | 260 × (`u8 present` + `[0x528]`) |
| 9 | player | `0x316ce` | 40921 | 1 | 8 слотов + две пары `0x50+0x3510` |
| 255 | EOF | `0x3b6a7` | 1 | — | конец файла, хвоста нет |

Блок 11 (SCRIPT) в сейве — рантайм-форма `gm.sbt`: `u32 things`, `things×40`,
`u32 named`, `named×12`, `u32 code_len`, `code`, `u8 flag`, при `flag` —
`9×0x400` таблиц. В `saved0`/`saved1`: things=10, named=351, code=62503,
flag=1 — это даёт 76344 байта payload.

## 2. API

```cpp
#include "res/save.h"

std::string error;
std::optional<gm::Save> save = gm::parse_save(bytes, error);
if (!save) { /* error: готовая строка с offset/размером */ }

const gm::SaveBlock* block = save->find_block(9);
if (save->player) {
    const gm::SavePlayer& p = *save->player;
    p.position;            // world X/Y/Z (double)
    p.armor / 100.0;       // сотые доли, kSaveGaugeScale
    p.shields;             // front/back/left/right, сотые
    p.weapon_slot; p.weapon_mask; p.ammo; // 12 слотов u32
}

std::optional<gm::SaveLevelMatch> match =
    gm::find_save_level(bytes, "/mnt/d/games/Gunmetal 3dfx", error);
```

* `parse_save(std::span<const std::uint8_t>, std::string&)` — заголовок, весь
  поток блоков, блок 9. Возвращает `nullopt` и заполняет `error` на любом
  нарушении границ/лимитов. Отсутствие блока 9 — не ошибка:
  `save->player == std::nullopt`.
* `SaveHeader` — `title`, `version` (сырой `+0x0c`), `reserved`, `level_name`,
  `fields[22]`, `name_slots[4]`.
* `SaveBlock` — `id`, абсолютные `offset`/`size` (включая байт id), `count`.
* `SavePlayer` — флаги 8 слотов, абсолютные смещения структур, число
  вложений, поля первого присутствующего слота, заголовки двух хвостовых пар,
  `has_slot`; хелперы `armor_points()`/`shield_points(i)` (делят на 100.0).
* `find_save_level(save, game, error)` — поиск исходного `.sbt` по якорям, как
  в `savefile.py`: 256 байт каждые 2048, пропуск чанков с ≥200 нулями,
  минимум 3 совпадения, побеждает файл с наибольшим числом совпадений.
  Возвращает имя файла, число совпадений, первый хит и `last_anchor`
  (эвристическая граница состояния из `savefile.py`).

Все чтения идут через bounds-checked `Reader`; счётчики ограничены теми же
лимитами, что `save_fields.py` (двери ≤100, переключатели ≤0x200, спрайты
≤0x1F3, звуки ≤0x7F, вещи 260, thing types ≤0x80, named items ≤0x31,
полигоны ≤0x1F3, объекты ≤0x12B, секторы ≤0xFF, лифты ≤0x1E, остановки
≤0xC8, SOB ≤0xC8, SCRIPT things ≤0xC8 / named ≤0x400 / code ≤0x30000),
блоков ≤32, файл ≤64 МиБ. Умножения count×stride выполняются в `size_t`
после проверки count, поэтому переполнения нет.

## 3. Блок игрока (9)

```
8 × { u8 present; если 1: 0x3510 структура + 16 байт на каждый ненулевой
      u32 из struct+0x317c }
2 × { char header[0x50]; 0x3510 структура }
```

В поставочных сейвах: present = `1,0,0,0,0,0,0,0`, вложений нет, заголовки пар
— `LEVEL0` и `""`. Поля первого слота (смещения внутри `0x3510`):

| смещ. | поле | saved0 | saved1 | единицы |
|---|---|---|---|---|
| `0x0000` | position.x | 1116.173751898651 | 1156.355385156283 | world |
| `0x0008` | position.y | 798.2076214059642 | 797.2758300679517 | world |
| `0x0010` | position.z | 48.0 | 48.0 | world |
| `0x0018` | angles[0] | 0.0 | 0.0 | Эйлер |
| `0x0020` | angles[1] | 0.0 | 0.0 | Эйлер |
| `0x0028` | angles[2] | 5.0736149946795805 | 4.747052494679579 | Эйлер |
| `0x3178` | exists | 7496160 | 7496160 | u32 |
| `0x3204` | history_index | 194 | 130 | ring 0..239 |
| `0x3210` | present | 1 | 1 | i32 |
| `0x3214` | object_id | 16 | 16 | i32 |
| `0x3244` | armor_max | 10000 | 10000 | сотые |
| `0x3248` | armor | 10000 | 10000 | сотые |
| `0x324c` | shield_max | 8000 | 8000 | сотые |
| `0x3250` | shields[4] | 8000 ×4 | 8000 ×4 | сотые |
| `0x3294` | weapon_slot | 0 | 1 | 0..11 |
| `0x3298` | weapon_mask | 3 | 3 | `1<<slot` |
| `0x329c` | ammo[0] | 2000 | 1975 | u32 |
| `0x32a0` | ammo[1] | 90 | 87 | u32 |
| `0x32a4..` | ammo[2..11] | 0 | 0 | u32 |

Границы (те же, что `save_fields.check_save`): `0 < armor_max ≤ 1000000`,
`0 ≤ armor ≤ armor_max`, `0 < shield_max ≤ 1000000`, `0 ≤ shields[i] ≤
shield_max`, `0 ≤ weapon_slot ≤ 11`; дополнительно позиция/углы должны быть
конечными (NaN/Inf отвергаются). Хвостовые пары не валидируются: у пары A в
поставочных сейвах `weapon_slot = -1`, это нормальное значение для
неиграбельной структуры.

## 4. Golden-таблица

Заголовок:

| поле | saved0 | saved1 |
|---|---|---|
| title | `A SAVED GAME` | `2` |
| version word `+0x0c` | `0x34` | `0` |
| reserved `+0x50` | 0 | 0 |
| level `+0x54` | `LEVEL0` | `LEVEL0` |
| F1 (level clock) | 4288 | 8148 |
| F0,F2..F21 | 1,1,0,0,0,64,0,90000,90000,0,0,0,0,4,2,1,1,6,6,0,0 | то же |
| name slots | `K-9`, ``, `coargus`, `none` | то же |
| размер файла | 243368 | 243368 |
| блоков | 15 | 15 |

Остальные поля игрока и все смещения блоков совпадают с таблицами §1 и §3
(они зашиты в `engine/tests/test_save.cpp`).

## 5. Что доказано, а что гипотеза

**Доказано на этих данных (2/2 сейва, два независимых читателя):**

* заголовок `0x13c`, имя уровня `+0x54`, 22 поля, 4 текстовых слота;
* порядок и границы всех 15 блоков: цепочка парсится от `+0x13c` до `0xff`
  без остатка, сумма размеров = 243368;
* блок 9: чередование `present` + `0x3510` (а не 8 флагов подряд), две
  хвостовые пары, поля брони/щитов/оружия/патронов;
* единицы брони/щитов — сотые (`10000 == 100.00`), патроны — штуки;
* эвристика якорей `savefile.py`: `level0.sbt`, 33 совпадения,
  `last_anchor = 0x1f384` у обоих сейвов.

**Гипотеза / не проверено:**

* смысл полей `angles[0..2]`: документация `docs/SAVE_FIELDS.md` §9.2
  называет `+0x20` yaw, но в обоих сейвах `+0x20 == 0`, а ненулевое значение
  лежит в `+0x28` (5.0736 / 4.7471). C++ API отдаёт три double без имён;
  расхождение с таблицей документа зафиксировано здесь, а не замазано.
* поля структур двух хвостовых пар: читаются только заголовки (`LEVEL0`, ``);
  тела пропускаются как raw, как и в `savefile.py`.
* блоки 4/5/11/12/15/16/17/20: проверяются только границы/размеры; тела
  (кроме counts блока 12) не декодируются. Побайтовый round-trip этих блоков
  доказан Python-инструментом `save_rebuild.py`, не этим кодом.
* блоки 1/2/6/7/8/19: тела — сырые рантайм-структуры, C++ их не разбирает.
* слоты игрока 1..7 и вложения: в поставочных сейвах отсутствуют, код
  поддержки написан, но на реальных данных не наблюдался (синтетический тест
  проверяет один слот с одним вложением).
* другие уровни: в установке только `LEVEL0`, поэтому другие сейвы не
  проверялись.

## 6. Ограничения

1. **Интеграция в `gm_engine` сделана основным агентом:** `src/res/save.cpp`
   добавлен в `add_library(gm_engine STATIC ...)`; CTest-тест `save` остаётся
   отдельным таргетом `gm_save_tests`.
2. **Только чтение.** Writer'а нет; `parse_save` не пересобирает байты.
3. **Строгая валидация.** Сейв с `armor_max == 0`, `weapon_slot == -1` в
   *присутствующем* слоте или NaN в позиции будет отвергнут; Python
   `savefile.py` в таких случаях просто не нашёл бы игрока. Для поставочных
   данных поведение совпадает.
4. **`find_save_level` зависит от установки.** Без `*.sbt` в каталоге игры
   функция возвращает ошибку; `parse_save` от установки не зависит.
5. `last_anchor` — эвристика `savefile.py`, лежит внутри записи 11; для
   реальных границ используется цепочка блоков от `+0x13c`.
6. Проверены только `saved0`/`saved1` (`LEVEL0`); блоки 16/18 на реальных
   данных не встречались.

## 7. Тест

`engine/tests/test_save.cpp` (CMake-таргет `gm_save_tests`, CTest-имя `save`,
label `quick`):

* golden-паритет обоих поставочных сейвов: заголовок, 22 поля, текстовые
  слоты, 15 блоков (id/offset/size/count), поля игрока, `find_save_level`;
* синтетический сейв: блоки 19 + 9 + 3, один слот с одним вложением —
  проверяются смещения, размеры, извлечение полей;
* негативные кейсы (ожидаемые тексты ошибок):
  * пустой/короткий файл — `too short`;
  * нет `0xff` — `no 0xff terminator`;
  * байт после `0xff` — `trailing bytes`;
  * огромный/неизвестный id — `unknown record type 200`;
  * обрезанный игрок — `truncated player struct`;
  * обрезанный хвост блока 9 — `truncated player sub struct`;
  * флаг присутствия 2 (игрок и things) — `presence flag 2 is not 0/1`;
  * `count = 0xffffffff` — `count ... out of range`;
  * двери `count = 101` — `out of range [0, 100]`;
  * дубликат блока — `duplicate block 19`;
  * непечатаемый заголовок / пустое имя уровня;
  * `armor_max = 0`, `armor > max`, щит > максимума, `weapon_slot = 12`;
  * обрезанный SCRIPT — `truncated script code`.

## 8. Воспроизведение

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sandbox --target gm_save_tests gm_tests -j 32
./build/sandbox/engine/tests/gm_save_tests      # save: ok
./build/sandbox/engine/tests/gm_tests           # gm_tests: all checks passed
ctest --test-dir build/sandbox -R '^save$' --output-on-failure
python3 regress.py                              # 106/106
```

Результаты прогона (2026-10-20):

```
save: ok
gm_tests: all checks passed
ctest save: 1/1 passed (5.14 s)
regress: 106/106 checks passed in 72.1s
```

Тот же тест под ASan+UBSan (`-fsanitize=address,undefined`, Debug) проходит:
`save: ok`, ни одного отчёта санитайзеров — OOB/переполнений на golden и
негативных кейсах нет.

Golden-значения сняты Python-инструментами:

```sh
python3 tools/re/savefile.py "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt" --json
python3 tools/re/save_fields.py --json
```
