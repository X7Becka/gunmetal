# C++: типизированные блоки сейва 1, 2, 3, 6, 7, 8

* **Статус:** блоки переключателей (1), дверей (2), things (3), спрайтов (6),
  источников звука (7) и `SECTOR_STUFF` (8) разбираются в структуры, пишутся
  обратно и проходят round-trip `saved0.sbt`/`saved1.sbt` байт-в-байт.
* **Ветка:** `feat/save-blocks`, база `master` `828074c`, дата 2026-10-20.
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx/SAVEGAME`).
* **Файлы:** `engine/src/res/save.h`, `engine/src/res/save.cpp`,
  `engine/tests/test_save_blocks.cpp`, регистрация теста в
  `engine/tests/CMakeLists.txt`.
* **Источники:** `docs/SAVE_FIELDS.md` §3–§6, §10, `tools/re/save_fields.py`
  (`parse_doors/parse_switches/parse_things/parse_sprites/parse_sounds`).

Существующий API не менялся: `SaveHeader`, `SaveBlock`, `SavePlayer`,
`parse_save()`, `Save::to_bytes()`, `rebuild_save()` и `find_save_level()`
работают как раньше. В `Save` добавлены векторы типизированных блоков, в
`SaveBlock::raw` по-прежнему лежит payload целиком (`raw.size() == size - 1`).

## 1. Что стало типизированным

| id | блок | структура | записей saved0/1 | размер записи |
|---|---|---|---|---|
| 1 | switches | `std::vector<SaveSwitch>` | 24 | `0x20` |
| 2 | doors | `std::vector<SaveDoor>` | 25 | `0xB8` |
| 3 | things | `std::array<bool,260> thing_present` + `std::vector<SaveThing>` | 6 (из 260 слотов) | `0x528` |
| 6 | sprites | `std::vector<SaveSprite>` | 4 | `0x2c` |
| 7 | sound sources | `std::vector<SaveSound>` | 6 | 28 байт на источник |
| 8 | `SECTOR_STUFF` | `std::vector<SbtSectorSlot>` | 256 слотов (13 ненулевых) | `0x58` |

`parse_save()` заполняет векторы в файловом порядке (things — по возрастанию
слота). `Save::to_bytes()` пишет блок из вектора, если вектор непустой
(для things — если выставлен хотя бы один флаг присутствия). Иначе
используется `SaveBlock::raw`, поэтому блок с `count == 0` и синтетические
`Save`, собранные вручную из raw-байтов, продолжают работать.

Границы те же, что в `save_fields.py`: 512 переключателей, 100 дверей,
499 спрайтов, 127 источников звука; for things — ровно 260 флагов.
`to_bytes()` отвергает вектор больше лимита, несогласованные флаги things и
слоты не по порядку.

## 2. Переключатели: блок 1, запись `0x20`

Все восемь dword'ов — дословные операнды `ADD_SWITCH`, запись целиком
покрыта полями.

| смещ. | поле | тип | статус | смысл |
|---|---|---|---|---|
| `0x00` | `kind` | i32 | proven | 0 обычный, `0x101` transformed, `0x102..0x104` source |
| `0x04` | `id` | i32 | proven | id сущности для `GENERATE` |
| `0x08` | `radius` | i32 | proven | радиус триггера для `0x102..0x104` |
| `0x0c` | `pos[0..2]` | 3 × i32 | proven | мировая позиция X/Y/Z |
| `0x18` | `aux` | i32 | hypothesis | вспомогательный звуковой аргумент |
| `0x1c` | `sound` | i32 | proven | индекс сэмпла, -1 = тишина |

## 3. Двери: блок 2, запись `0xB8`

| смещ. | поле | тип | статус | смысл |
|---|---|---|---|---|
| `0x00` | `box[0..8]` | 9 × f64 | proven | закрытый угол (3), открытый угол (3), размер (3) |
| `0x48` | `delta[0..2]` | 3 × f64 | proven | движение бокса за тик |
| `0x60` | `constant` | f64 | unknown | константа `0.03` из `ADD_DOOR`, читателя нет; хранится как поле |
| `0x68` | `flags` | i32 | proven | тип в младшем ниббле (1..3), бит `0x40` — используемая дверь |
| `0x6c` | `ids[0..4]` | 5 × i32 | proven | пять операндов E |
| `0x80` | `state` | i32 | proven | -1 закрывается, 0 закрыта, +1 открывается |
| `0x84` | `sector` | i32 | proven | индекс сектора в `.sbw` |
| `0x88` | `object` | i32 | proven | индекс объекта в секторе |
| `0x8c` | `phase_prev` | u32 | proven | фаза до тика, 0..`0xffff` |
| `0x90` | `phase_next` | u32 | proven | фаза после тика |
| `0x94` | `step` | i32 | proven | приращение фазы за тик |
| `0x98` | `param5` | i32 | unknown | операнд L6, читателя нет |
| `0x9c` | `param6` | i32 | unknown | операнд L7, читателя нет |
| `0xa0` | `v[0..2]` | 3 × i32 | hypothesis | три V хвоста `VNVNVN` |
| `0xac` | `n[0..2]` | 3 × i32 | hypothesis | три N хвоста `VNVNVN` |

Неизвестные `0x60/0x98/0x9c` и гипотетические `v/n` типизированы как поля,
поэтому правка любой доказанной двери не задевает их байты.

## 4. Things: блок 3, слот `0x528`

Формат — `260 × (u8 present + [0x528])`, флаги и записи **чередуются**.
Поля внутри `0x528` в карте не доказаны, поэтому структура хранит слот и
сырые байты:

```cpp
struct SaveThing {
    std::size_t slot;                              // 0..259
    std::array<std::uint8_t, 0x528> raw;           // без интерпретации
};
```

`Save` дополнительно хранит `thing_present[260]`, чтобы writer мог
восстановить все флаги, включая нулевые. Правка `raw` меняет ровно
соответствующие байты записи; правка `slot`/флагов проверяется на
согласованность при записи.

## 5. Спрайты: блок 6, запись `0x2c`

| смещ. | поле | тип | статус |
|---|---|---|---|
| `0x00` | `scale_x` | f32 | proven |
| `0x04` | `scale_y` | f32 | proven |
| `0x08` | `runtime_object` | u32 | proven (устаревший указатель, пересоздаётся) |
| `0x0c` | `pos[0..2]` | 3 × i32 | proven |
| `0x18` | `param` | u8 | hypothesis |
| `0x19` | `reserved0[3]` | 3 × u8 | не отображено, сохраняется |
| `0x1c` | `flags` | u32 | hypothesis |
| `0x20` | `effect_id` | i32 | proven |
| `0x24` | `param16` | u16 | hypothesis |
| `0x26` | `reserved1[2]` | 2 × u8 | padding, сохраняется |
| `0x28` | `spawn_time` | u32 | proven |

## 6. Источники звука: блок 7

Файл хранит **шесть параллельных колонок**, а не массив записей:
`n × i32 x`, `n × i32 y`, `n × i32 z`, `n × i32 volume`, `n × i32 sample`,
`n × f64 pitch` (так же читает `parse_sounds` в `tools/re/save_fields.py`).
Структура `SaveSound` — логическая запись источника; writer снова раскладывает
её по колонкам.

| колонка | поле | тип | статус |
|---|---|---|---|
| x | `x` | i32 | proven |
| y | `y` | i32 | proven |
| z | `z` | i32 | proven |
| volume | `volume` | i32 | proven |
| sample | `sample` | i32 | proven |
| pitch | `pitch` | f64 | proven |

## 7. Counts и значения поставочных сейвов

| величина | saved0 | saved1 |
|---|---|---|
| switches (блок 1) | 24 | 24 |
| doors (блок 2) | 25 | 25 |
| things: присутствующих слотов | 6 (слоты 0..5) | 6 (слоты 0..5) |
| sprites (блок 6) | 4 | 4 |
| sound sources (блок 7) | 6 | 6 |
| размер файла | 243368 | 243368 |
| FNV-1a 64 | `0xb994929515f53af5` | `0x0d038197e152cd65` |

Контрольные значения, закреплённые тестом:

* дверь 0: `box[0..2] = 664/928/-4`, `flags=3`, `ids[0]=2`, `sector=4`,
  `object=1`, `step=700`, `param5=300`, `v={64,64,64}`, `n={132,133,134}`;
  дверь 20: `delta[0]=3.19584207277515e-05`, `step=800`, `n={-1,50,135}`;
* переключатель 0: `kind=0`, `id=-30240`, `radius=160`,
  `pos={-600,928,-88}`, `sound=-1`; переключатель 5: `kind=0x101`,
  `id=9`, `pos={1,0,4}`; переключатель 23: `kind=0x104`, `id=-30221`;
* спрайты: `scale=1.5/1.5`, `param=24`, `flags=2`, `effect_id=363`,
  `param16=4`, `spawn_time=0`, позиции `{15,328,143}`, `{31,920,143}`,
  `{15,0,127}`, `{15,-160,127}`;
* звуки: `{x=-616,y=-168,z=71,volume=19,sample=128,pitch=0.05}` …
  `{x=1552,y=608,z=23,volume=19,sample=114,pitch=0.1}`;
* things: во всех шести слотах `raw` непустой; saved0 и saved1 отличаются
  только слотом 0 (86 байт, см. `docs/SAVE_FIELDS.md` §11).

Блоки 1/2/6/7 между saved0 и saved1 совпадают полностью: все записи
идентичны.

## 8. Byte-exact

`parse_save() -> Save::to_bytes()` для обоих файлов даёт исходные 243368
байт; `rebuild_save(save, verify=true)` возвращает те же байты, пересобирает
смещения/raw и проверяет, что типизированные поля всех пяти блоков пережили
round-trip (`same_typed_blocks`). Тест `gm_save_blocks_tests` проверяет:

* размер, FNV-1a 64 и полное побайтовое равенство;
* `block.raw.size() == block.size - 1` для каждого блока;
* counts и контрольные поля выше;
* `rebuild_save(..., true)` без ошибок.

## 9. Правки полей

На `saved0.sbt` тест меняет по одному полю каждого типа и проверяет, что
изменились только байты этих полей (размер файла тот же, все изменения
внутри ожидаемых диапазонов):

| поле | смещение в файле | изменилось байт |
|---|---|---|
| `switches[3].sound` (`127 -> 7`) | `block1 + 1 + 4 + 3*0x20 + 0x1c` | 1 |
| `doors[12].step` (`99999 -> 123`) | `block2 + 1 + 4 + 12*0xB8 + 0x94` | 3 |
| `sprites[2].pos[0]` (`15 -> 99`) | `block6 + 1 + 4 + 2*0x2c + 0x0c` | 1 |
| `sounds[4].volume` (`19 -> 42`) | `block7 + 1 + 4 + 3*4*n + 4*4` (колонка volume) | 1 |
| `things[1].raw[0x40] ^= 0x5a` | `block3 + 1 + (1 + 0x528) + 0x40` | 1 |

Здесь `blockN` — смещение байта id блока в файле, `n` — число источников
звука (6).

После `rebuild_save` повторный разбор видит новые значения, а
`to_bytes()` повторно даёт те же пропатченные байты.

## 10. Негативные проверки

`gm_save_blocks_tests` собирает битые файлы из `saved0` в памяти:

| кейс | ожидаемая ошибка |
|---|---|
| `n` дверей 101 | `count 101 out of range [0, 100]` |
| `n` переключателей 513 | `count 513 out of range` |
| `n` спрайтов 500 | `count 500 out of range` |
| `n` звуков 128 | `count 128 out of range` |
| обрезанная запись двери/переключателя/спрайта/звука | `truncated ... record` |
| «плохой шаг»: `count=1`, но перед `0xff` только 183 байта | `record stream has no 0xff terminator` |
| флаг things = 2 | `presence flag 2 is not 0/1` |
| обрезанная things-запись | `truncated thing record` |
| writer: 101 дверь в модели | `doors: count 101 out of range` |
| writer: 7 флагов things и 6 записей | `things: 6 records for 7 presence flags` |
| writer: things-запись не своему слоту | `expected slot 0` |

## 11. Проверка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_save_blocks_tests -j 32
ctest --test-dir build/sandbox -R save --output-on-failure
python3 regress.py
```

`ctest -R save` гоняет старые `save`/`save_writer` и новый `save_blocks`;
`regress.py` не менялся и по-прежнему проверяет Python-эталон.

## 12. Что осталось

1. **Внутренние поля `0x528` things.** Сейчас запись целиком raw; в карте
   (`docs/SAVE_FIELDS.md` §10, §14.3) для неё нет доказанных полей. Нужен
   разбор потребителей блока 3 в `gundx.exe`.
2. **Потребители гипотез:** `SaveSwitch::aux`, `SaveSprite::param/flags/
   param16`, `SaveDoor::param5/param6/v/n`. Писатель и раскладка известны,
   игровой читатель — нет.
3. **Запуск игры с пропатченным файлом.** Доказан round-trip и повторный
   разбор нашим читателем; проверка загрузки оригинальным `gundx.exe`
   требует копии `SAVEGAME` и не должна менять установку.
4. **Блоки 4/5/11/12/15/17/20** остаются raw: их кодеки уже есть в `gm.sbt`
   и `docs/RECORDS.md`, но в C++-писатель не переносились.
5. **Подключение писателя к CLI/движку** — вне этой задачи; движок только
   читает сейвы.

## 13. Блок 8 `SECTOR_STUFF`: типизированный дамп рантайм-массива

Блок 8 — единственный из «крупных», который не имеет счётчика: ровно
`0x5800 = 256 × 0x58` байт, то есть побайтовый дамп рантайм-массива
`0x800b60` (загрузчик `0x46b7b0` — `fread` целиком, писатель `0x46b7f0` —
`0x481020(id)` + `fwrite` целиком). Поэтому типизация здесь — это
`res::SbtSectorSlot` (все `0x58` байт покрыты полями) и
`parse_sector_stuff`/`encode_sector_stuff` из `engine/src/res/sbt.{h,cpp}`;
блок 8 в `Save` — `std::vector<SbtSectorSlot> sector_stuff` (256 записей,
пустой вектор = блока нет).

Полная карта полей, адреса писателей/читателей и семантика команд
`SET_SECTOR_*` — `docs/RECORDS.md` §2. Здесь только то, что касается сейва:

| что | значение |
|---|---|
| смещение блока в `saved0`/`saved1` | `0x29e2b` (id), payload `0x29e2c .. 0x2a62b` |
| `SaveBlock::count` | `0` (фиксированный размер) |
| payload FNV-1a 64 | `0x19fa6a27ba4b0c33` (в обоих файлах одинаков) |
| ненулевых слотов | 13: 5, 9, 11, 13, 14, 19, 34, 35, 48, 50, 51, 54, 57 |
| round-trip | `encode(parse(x)) == x`, `to_bytes()` == исходный файл |

Писатель берёт блок из `sector_stuff`, если вектор непустой (fallback на
`SaveBlock::raw`, как у остальных блоков); `rebuild_save(verify)` сверяет
поля через `same_typed_blocks`. Тест — `engine/tests/test_save_sector_stuff.cpp`
(`ctest -R save_sector_stuff`): сверка с Python-оракулом `gm/sbt.py`
`SectorSlot`/`tools/re/records.py`, правка поля по точному смещению,
нулевой блок, обрезка блока и неверное число слотов.
