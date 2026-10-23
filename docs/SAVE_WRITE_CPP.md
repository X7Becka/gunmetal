# C++-писатель сейва: `write_save()`

* **Статус:** готово. Поставочные `SAVEGAME/saved0.sbt` и `saved1.sbt` проходят
  `parse_save -> write_save -> bytes` байт-в-байт; байты совпадают с
  Python-писателем `tools/re/save_rebuild.py` на нетронутых файлах **и** на
  файле с правкой брони/щита/аммо.
* **Пункты roadmap:** `M8.7` (писатель сейва), `M9.4` (сохранение состояния).
* **Ветка:** `feat/saveload`, база `master` `2d9cd62`.
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx/SAVEGAME`).
* **Новые файлы:** `engine/src/res/save_write.h`, `engine/src/res/save_write.cpp`,
  `engine/tests/test_save_write.cpp`, этот документ.
* **Правки только в:** `engine/CMakeLists.txt` (`save_write.cpp` в `gm_engine`),
  `engine/tests/CMakeLists.txt` (тест `save_write`, label `quick`).
* **Опоры:** `docs/SAVE.md` (контейнер), `docs/SAVE_BLOCKS.md` (типы блоков),
  `docs/SAVE_FIELDS.md` (§2 заголовок, §9 блок 9), `docs/SAVE_WRITER.md`
  (API `Save::to_bytes()`/`rebuild_save()`), `docs/SAVE_CPP.md` (читатель),
  `tools/re/save_rebuild.py` (Python-эталон).

## 1. Что это за модуль и почему он отдельный

Раскладка и кодировщик уже есть в `res/save.cpp`: `Save::to_bytes()` пишет
заголовок из типизированных полей, блоки 1/2/3/6/7/8/9 — из типизированных
векторов, все остальные — из `SaveBlock::raw` (см. `docs/SAVE_WRITER.md` §2–§3,
`docs/SAVE_BLOCKS.md` §1). Второй экземпляр тех же 200+ строк со смещениями
здесь был бы только местом для расхождения, поэтому `save_write.cpp` —
**строгий вход** в тот же кодировщик:

```cpp
#include "res/save_write.h"

gm::SaveGame game = *gm::parse_save(bytes, error);   // SaveGame = Save
game.header.title  = "AFTER BOSS";
game.player->armor = 4200;

std::vector<std::uint8_t> out;
std::string error;
gm::SaveWriteCode code = gm::SaveWriteCode::ok;
if (!gm::write_save(game, out, &error, &code, bytes)) {
    // error = "write_save: block 17: ...", code = SaveWriteCode::block_bounds
}
```

`write_save()` добавляет к `to_bytes()` четыре вещи, которых у кодировщика нет
(и не должно быть — он остаётся чистым сериализатором):

1. **Проверку модели до кодирования.** Цепочка блоков обязана быть
   непрерывной от `kSaveHeaderSize` (0x13c), `offset`/`size` — внутри
   `Save::size`, `raw.size() == size - 1`, без дублей/`0xff`/неизвестных id,
   блок 9 присутствует ровно тогда, когда есть `Save::player`.
2. **Коды ошибок** (`SaveWriteCode`) рядом с текстом.
3. **Перенос opaque-байтов заголовка** (`0x10..0x50`) из исходного файла:
   без него писатель канонизирует их в нули (в поставочных сейвах они и так
   нулевые, поэтому round-trip точен и без него).
4. **Обязательный re-parse результата** с типизированным сравнением
   (заголовок, порядок/id/offset/size блоков, поля игрока). `true` от
   `write_save()` означает «эти байты разбираются обратно в ту же модель».

`out` очищается на каждом вызове и заполняется только при успехе, так что
после отказа наружу не уходит полусобранный файл.

## 2. Раскладка: что пишется и что остаётся opaque

Контейнер — `docs/SAVE.md` §1: `0x13c` заголовка, затем поток
`[u8 id][payload]...`, затем `0xff`. Ни выравнивания, ни контрольной суммы,
ни хвоста.

### 2.1 Заголовок (0x13c)

| смещ. | длина | что | источник при записи |
|---|---|---|---|
| `0x00` | 12 | title | `SaveHeader::title`, печатный ASCII, ≤ 12 байт |
| `0x0c` | 4 | version word | `SaveHeader::version` (в `saved0` = 0x34 — на самом деле хвост title, см. `SAVE.md` §1.1) |
| `0x10` | `0x40` | **opaque**, читатель не смотрит | из `original`, иначе нули (§3) |
| `0x50` | 4 | reserved | `SaveHeader::reserved` |
| `0x54` | `0x50` | имя уровня | `SaveHeader::level_name` (`SAVE.md` §1.1) |
| `0xa4` | `0x58` | `F0..F21` | `SaveHeader::fields` |
| `0xfc` | `0x40` | 4 слота `K-9`/`""`/`coargus`/`none` | `SaveHeader::name_slots` |

### 2.2 Блоки

Порядок — как в `Save::blocks` (то есть как в исходном файле), без
канонической сортировки. `docs/SAVE_BLOCKS.md` §1:

| id | блок | как пишется | ссылка |
|---|---|---|---|
| 1 | switches | типизированно из `Save::switches` (24 записи `0x20`) | §2 |
| 2 | doors | типизированно из `Save::doors` (25 × `0xB8`) | §3 |
| 3 | things | типизированно из `thing_present[260]` + `things` (6 слотов `0x528`) | §4 |
| 4 | thing types | **opaque** (`SaveBlock::raw`) | §1 |
| 5 | named items | **opaque** | §1 |
| 6 | sprites | типизированно из `Save::sprites` (4 × `0x2c`) | §5 |
| 7 | sound sources | типизированно из `Save::sounds` (6, колонки) | §6 |
| 8 | `SECTOR_STUFF` | типизированно из `Save::sector_stuff` (256 × `0x58`) | §13, `docs/RECORDS.md` §2 |
| 9 | player | типизированно из `SavePlayer` (1 присутствующий слот) | `SAVE_FIELDS.md` §9 |
| 11 | script | **opaque** | §1 |
| 12 | sound table | **opaque** | §1 |
| 15 | poly handles | **opaque** | §1 |
| 16 | object handles | **opaque** | §1 |
| 17 | sector handles | **opaque** | §1 |
| 18 | lifts | **opaque** | §1 |
| 19 | ITEM | **opaque** | §1 |
| 20 | SOB table | **opaque** | §1 |

«Opaque» — это не потеря данных: `parse_save()` кладёт payload целиком в
`SaveBlock::raw` (инвариант `raw.size() == size - 1`), писатель копирует его
байт-в-байт. Поэтому блоки, для которых в C++ нет типизированного вида,
возвращаются ровно такими, какими пришли; их нельзя *править*, но можно
переносить.

Блок 9: `SavePlayer` хранит `slot_raw[8]` (сырые `0x3510` каждого слота),
`attachment_raw[8]` (записи по 16 байт), `pair_header_raw[2]` /
`pair_struct_raw[2]` (две хвостовые пары). Правятся только типизированные
поля **первого присутствующего** слота по смещениям `0x0000`, `0x0018`,
`0x3178`, `0x3204`, `0x3210`, `0x3214`, `0x3244`, `0x3248`, `0x324c`,
`0x3250`, `0x3294`, `0x3298`, `0x329c`; всё остальное — из `slot_raw`.
Габариты (`armor_max`, `shield_max`, броня/щиты, `weapon_slot`, конечность
позиции/углов) проверяются так же, как в читателе.

## 3. Opaque-байты заголовка

Байты `0x10..0x50` (между словом version и `reserved`) `parse_save()` не
читает. Канонический писатель (`Save::to_bytes()`, `save_rebuild.py`) пишет
там нули. В обоих поставочных сейвах они и есть нули, поэтому это не видно,
но для чужого/модифицированного сейва с ненулевым padding такой файл
перестал бы быть байт-идентичным.

`write_save(game, out, &error, &code, original)` копирует эти 64 байта из
`original` (исходный файл, `original.size() == game.size`) в результат.
Тест это доказывает: в `saved0.sbt` эти 64 байта заполняются шаблоном
`0xa0..0xdf`, файл всё ещё разбирается (поля заголовка те же), и
`write_save` с `original` возвращает **ровно** пропатченные байты, а без
`original` — исходный `saved0.sbt` с нулями там.

Ограничение: остальные «щели» заголовка (хвосты строк после NUL, padding
внутри `name_slots`) не сохраняются — они нормализуются в нули. В
поставочных сейвах они нулевые, так что round-trip точен; сейв с мусором в
хвосте строки будет канонизирован (это то же поведение, что у
`Save::to_bytes()` и у Python-писателя).

## 4. Коды ошибок

`const char* save_write_code_name(SaveWriteCode)` — стабильные имена.

| код | когда | пример текста |
|---|---|---|
| `ok` | запись удалась | — |
| `no_blocks` | `Save::blocks` пуст | `the model has no blocks; ...` |
| `block_bounds` | смещение/размер/`size` вне файла, `raw.size() != size - 1`, нет `0xff` в конце, `original` другого размера | `block 17 ends at 0x13c+..., past the 0x3b6d8-byte file` |
| `block_layout` | дубль id, id `0xff`, неизвестный id, нет raw-payload | `duplicate block 17` |
| `missing_player` | блок 9 без `Save::player` или наоборот | `the chain has block 9 but the model carries no player state` |
| `not_representable` | поле не кодируется (title > 12, level > 0x50, броня > максимума, NaN в позиции, count больше лимита) | `save title is 13 bytes, maximum is 12` |
| `too_large` | результат > 64 МиБ | `rebuilt save is too large (> 67108864 bytes)` |
| `no_round_trip` | записанные байты не разбираются обратно в ту же модель | `header did not survive the round-trip` |

Текст всегда начинается с `write_save: `.

## 5. Числа: поставочные сейвы

Тест `save_write` (`ctest -R save_write`), вывод:

```
save_write: saved0.sbt 243368 bytes sha256 e39c0c35c44af6b985cb272f52b1236404d4316b266dffc7ccf0548503b623ff fnv 0xb994929515f53af5 -> identical (15 blocks)
save_write: saved1.sbt 243368 bytes sha256 93f4edd3fad07b31f47f392acf0c409f4e9217efcc521052ca8e74966834bd13 fnv 0x0d038197e152cd65 -> identical (15 blocks)
save_write: edits ok (title 1 run(s), vitals 3 run(s), size 243368)
save_write: 125 checks, ok
```

| файл | размер до | размер после | sha256 до = после | FNV-1a 64 | блоков |
|---|---|---|---|---|---|
| `saved0.sbt` | 243368 | 243368 | `e39c0c35…b623ff` | `0xb994929515f53af5` | 15 |
| `saved1.sbt` | 243368 | 243368 | `93f4edd3…34bd13` | `0x0d038197e152cd65` | 15 |

Проверяется побайтовое равенство (`written == shipped`), совпадение
size/sha256/fnv до и после, наличие 15 блоков и повторный разбор результата.
SHA-256 считается прямо в тесте (свой компактный FIPS 180-4), поэтому числа
воспроизводимы без внешних утилит; `sha256sum` на установке даёт те же
значения.

## 6. Числа: правка затрагивает только целевые байты

На `saved0.sbt` (смещение `slot_offsets[0]` — начало `0x3510` слота):

| правка | диапазон изменяемых байт | изменилось прогонов |
|---|---|---|
| `header.title = "GM WRITE"` | `[0x00, 0x0c)` | 1 |
| `player->armor = 5000` | `[slot+0x3248, slot+0x324c)` | 1 |
| `player->shields[2] = 2000` | `[slot+0x3258, slot+0x325c)` | 1 |
| `player->ammo[3] = 7` | `[slot+0x32a8, slot+0x32ac)` | 1 |

Тест строит список отличающихся прогонов `diff_runs()` и требует, чтобы
каждый прогон лежал **внутри** одного из ожидаемых диапазонов; размер файла
не меняется, повторный `parse_save` видит новые значения, остальные поля
позиции/углов/щитов/аммо и все блоки кроме 9 совпадают с исходными
байт-в-байт. Для заголовка отдельно проверяется, что изменились только
байты title, а уровень и `F0..F21` те же.

## 7. Паритет с Python-писателем

`tools/re/save_rebuild.py` — независимый эталон (`docs/SAVE_WRITER.md` §8).
Сравнение на **одном и том же входе** (обе команды пишут в свои каталоги):

```sh
./build/sandbox/engine/tests/gm_save_write_tests --emit /var/tmp/savewrite/cpp
python3 tools/re/save_rebuild.py --out /var/tmp/savewrite/py --force \
    "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt" \
    "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved1.sbt"
cmp /var/tmp/savewrite/cpp/saved0.sbt /var/tmp/savewrite/py/saved0.sbt && echo same
cmp /var/tmp/savewrite/cpp/saved1.sbt /var/tmp/savewrite/py/saved1.sbt && echo same
```

Фактический вывод (2026-10-24):

```
emit /var/tmp/savewrite/cpp/saved0.sbt 243368 bytes sha256 e39c0c35…b623ff
emit /var/tmp/savewrite/cpp/saved1.sbt 243368 bytes sha256 93f4edd3…34bd13
emit /var/tmp/savewrite/cpp/saved0-edit.sbt 243368 bytes sha256 30684516…f9262bc92 (armor 5000, shield 2000, ammo 7)
saved0.sbt -> /var/tmp/savewrite/py/saved0.sbt
saved1.sbt -> /var/tmp/savewrite/py/saved1.sbt
saved0: cpp == py
saved1: cpp == py
saved0-edit: cpp == py
```

Правка на стороне Python (тот же вход, те же смещения `0x3248`,
`0x3250+2*4`, `0x329c+3*4`):

```sh
python3 - <<'PY'
import struct, sys
sys.path.insert(0, "tools/re")
import save_rebuild as sr
s = sr.load("/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt")
i = s.blocks[9].present.index(1)
st = bytearray(s.blocks[9].structs[i])
struct.pack_into("<i", st, 0x3248, 5000)
struct.pack_into("<i", st, 0x3250 + 2 * 4, 2000)
struct.pack_into("<I", st, 0x329c + 3 * 4, 7)
s.blocks[9].structs[i] = bytes(st)
open("/var/tmp/savewrite/py/saved0-edit.sbt", "wb").write(s.to_bytes())
PY
cmp /var/tmp/savewrite/cpp/saved0-edit.sbt /var/tmp/savewrite/py/saved0-edit.sbt
sha256sum /var/tmp/savewrite/cpp/saved0-edit.sbt   # 30684516…f9262bc92
```

`--emit` пишет именно те байты, которые вернул `write_save()`; ключ нужен
только для этой сверки и в игре не используется.

Отдельный, ещё более независимый побайтовый патчер — `gm.py save --set`
(`tools/re/savefile.py::patch`): он правит байты на месте, без пересборки
контейнера. Его проверки живут в `regress.py` (`save / --set writes a patched
copy`, `save / --set rejects ammo above the limit`); в этой задаче сверка шла
через `save_rebuild.py`, потому что он даёт полный файл на выходе.

## 8. Ограничения

1. **Мир `.sbw` не пишется.** Сейв — это `savedN.sbt` (+ отдельный
   `savedN.sbw` в поставочной установке). Модуль пишет только `.sbt`;
   соответствующий `.sbw` остаётся от исходного слота. Для полноценного
   сохранения новой игры нужен писатель `.sbw` (отдельная задача,
   `docs/WORLD_FIELDS.md`).
2. **Нет сжатия и нет архивации.** Формат плоский; `write_save` не упаковывает
   и не сжимает.
3. **Неизвестные блоки только сохраняются.** Блоки 4/5/11/12/15/16/17/18/19/20
   переносятся из `SaveBlock::raw`; их поля нельзя править, пока для них нет
   типизированного вида (`docs/SAVE_BLOCKS.md` §12).
4. **Типизирован только первый присутствующий слот игрока.** Модели с 8
   слотами пишутся (тест `save_writer` это покрывает), но правятся поля
   первого; остальные слоты и обе хвостовые пары — raw.
5. **Вложения игрока (`attachment_raw`) не редактируются.** Число записей
   должно совпадать с числом ненулевых id в `struct+0x317c`.
6. **Заголовок не хранится raw.** Мусор в padding/хвостах строк
   канонизируется в нули; спасает только `original` для `0x10..0x50` (§3).
7. **`write_save` требует модель из `parse_save()`.** `Save::size`,
   `block.offset`, `block.size` должны описывать исходный файл; модель,
   собранную с нуля без этих полей, он отвергнет (`block_bounds`). Это
   осознанно: цель — доказуемый round-trip, а не конструктор контейнера
   (для сборки с нуля есть `to_bytes()`).
8. **Загрузка оригинальным `gundx.exe` не проверена.** Доказан round-trip и
   повторный разбор нашим читателем + совпадение с Python-эталоном; запуск
   игры с пропатченным файлом требует копии `SAVEGAME` и не входит в задачу.
9. **К CLI/движку писатель не подключён.** Только библиотека + тест;
   `main.cpp` не менялся.

## 9. Проверка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DGM_BUILD_TESTS=ON -DGM_WITH_SDL3=ON \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox -j 4
ctest --test-dir build/sandbox -R save_write --output-on-failure
ctest --test-dir build/sandbox -j 4
python3 regress.py --mirror --json
```

Тест `save_write` (label `quick`) не требует установки: при её отсутствии
гоняются только синтетические негативы (сейв без блоков, битая цепочка),
остальное печатает `shipped saves not installed`.

Результат прогона 2026-10-24 (эта ветка): `ctest --test-dir build/sandbox -j 4`
— **175/175 passed** (372.76 с, `full` 783.48 с*proc, `quick` 242.04 с*proc);
`python3 regress.py --mirror --json` — `{"passed": 115, "total": 115,
"seconds": 62.6, "failed": []}`.
