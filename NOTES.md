# Gunmetal (1998, Mad Genius Software) — разбор

Рекогносцировка данных из `D:\games\Gunmetal 3dfx` (4211 файлов, 388 МБ).
Версия: **1.36** (патч от 27.11.1998, `patch136.txt`). Движок: **FSDS Game System 1.14**.

## 1. Исполняемые файлы

| Файл | Формат | Комментарий |
|---|---|---|
| `gun.exe` | MS-DOS LE, DOS/4GW | 3dfx-сборка, 3.0 МБ, Watcom C/C++ 16-битный runtime |
| `gundx.exe` | PE32 GUI (i386) | DirectX-сборка, 2.7 МБ, Watcom, 21 секция |
| `dos4gw.exe` | DOS4G extender | нужен для `gun.exe` |
| `DOSWINKY.EXE` | PKWARE PKSFX | самораспаковывающийся архив |

Проверено:
- `objdump -d gundx.exe` работает (обычный PE32, пролог SEH от Watcom).
- `objdump` **не** понимает `gun.exe` (LE-формат) — нужен свой парсер LE или
  отдельный дизассемблер (Ghidra/radare2).
- `gundx.exe` импортирует `DDRAW.DLL` (`DirectDrawCreate`), `DINPUT.DLL`
  (`DirectInputCreateA`), `DSOUND.DLL` (`DirectSoundCreate`), `KERNEL32`,
  `USER32`, `GDI32`, `ole32`.
- `gun.exe` грузит `GLIDE2X.OVL` через `grGlideInit` / `grGlideShutdown` /
  `grGlideGetVersion` / `grGlideSetState` / `grGlideGetState` /
  `grGlideShamelessPlug`. Переключатели `/glide` и `/nglide`.
- ⚠️ **`GLIDE2X.OVL` в каталоге отсутствует** — DOS-сборка в Glide здесь не
  заведётся (осядет на софтверный рендер). Играбельный путь —
  `gundx.exe` + обёртки: `DDraw.dll`/`D3DImm.dll` (dgVoodoo) и каталог `DXWnd`.
- Строки `gun.exe` и `gundx.exe` совпадают на ~1461 из ~1900 уникальных строк —
  кодовая база общая, различаются только графические бэкенды.

## 2. Форматы данных (по проверенным фактам)

### Текстовые (разбираются сразу)

| Расш. | Что | Проверено |
|---|---|---|
| `.sob` | геометрия объекта: кол-во вершин, кол-во полигонов, вершины `x y z`, имя текстуры, 7×`none`, 6 индексов, матрица (15 чисел) | да, `SOBS/BOX1.SOB` |
| `.soe` | определения «thing» (сущностей): версия 0..13 + блоки коллизии (`PUSHABLE`/`ELLIPSOID`), событий, оружия, детей; грузятся как `THING\<name>.soe` (`0x47cb6c`) и компилируются в запись `THING_TYPES` файла `.sbt` | **чтение+запись**: `to_bytes` побайтово точен; грамматика снята с загрузчика (`SOBS/railrock.soe` — 67/67 токенов) |
| `.soa` | анимации: заголовок `1 48 6 1` + список имён кадров | да, `anims/DNA.SOA` |
| `.sml` / `.sme` | UI-разметка в стиле XML (`<d><c>…`) и тексты брифингов; в `.sme` текст закодирован подстановкой/шифром | да |
| `.sub` | списки подстановок текстур/звуков (`119`, `20 alumnm4 alumnm5`) | да |
| `.slu`, `.sni`, `.pin`, `.key`, `.fdx`, `.cfg`, `.hi`, `.txt` | конфиги/списки/темы, обычный текст | да |

### Бинарные

| Расш. | Заголовок / структура | Статус |
|---|---|---|
| `.sbt` | `BINARY SAVE\0…` — скомпилированная текстовая «World Things file»: записи с 1-байтовым тегом = id ключевого слова (`POLY_HANDLE`=15 и т. д.). Таблицы handle — по колонкам `(tag, sector, object, poly)`, где `(sector,object,poly)` есть адрес полигона движка. Запись `SCRIPT` (11) содержит **байткод** (опкоды, таблицы команд, `IF`/`ELSE`/`END`, `DEFINE_*`-таблицы) | **чтение+запись**: `Sbt.to_bytes` даёт побайтовый round-trip всех 36 файлов (`--rebuild`); `encode_payload` точно пересобирает handle-таблицы, `NAMED_ITEMS`, `THING_TYPES`, `SOB_TABLE` (205/205); **скриптовая ВМ разобрана** — `gm.sbtvm`, 36/36 декодируются до байтового конца, имена опкодов сняты с текстового загрузчика |
| `.sbw` | `u32 count` + таблица текстур `{name\0, u16, u32}` + таблица эффектов `{name\0, u32 kind, u32 start, u32 n, u16 ids[n]}` + `u32 count3` + стадия 1 (секторы/объекты/шапки) + стадия 2 (вершины + полигоны) | **чтение+запись**: `Sbw.to_bytes` даёт побайтовый round-trip всех 36 файлов (CLI `--rebuild`); декодеры `gundx.exe`: `0x42aa63`/`0x42ad65`/`0x425f4c`/`0x434463` |
| `.sbc` | `char magic[4]` (`SBC1` 46 шт. / `SBC0` 5 шт.) + `u32 count` + `count × { u32 desc; u8 пиксели }`, где `w = 1 << (desc & 0xffff)`, `h = 1 << (desc >> 16)` | **чтение+запись** (`Sbc.to_bytes` побайтово точен на всех 51 файле); данные — гладкие 8-битные карты, похоже на атласы лайтмапов |
| `.sbv` | видео: `SBV0` + `u32 width; u32 height; u32 frame_count` + таблица `{u32 timestamp_ms; u32 end_offset}` | **разобран полностью вместе с кодеком** (по дизасму `gundx.exe` `0x49e1e8`/`0x49ee8c`); побайтовая сверка с эмулятором на 25 кадрах (см. §2b) |
| `.sbm` | 12-байтный заголовок + палитровая 8-битная mip-цепочка | **чтение+запись**: `to_bytes` даёт побайтовый round-trip всех 1827 блобов; `sbm.encode` + CLI `gm.py sbm` (PNG/BMP → .sbm, флаги 0/1/2/16 у текстур, 256/257 у спрайтов); палитра в блобе не хранится |
| `.sou` | контейнер `SOU0/SOU1` (16-байтный заголовок) и `SOU2` (20-байтный) | **контейнер разобран** на всех 999 записях; кодек `SOU1/2` — драйверный (Miles/AIL), из установки не декодируется (см. ниже) |
| `.dat` + `.idx` | архивы **BIGFILE**: `.idx` = `"BIGFILE" 0x1A u32 count`, далее записи `{char name[8], u32 offset}`; `.dat` = сырые блобы | **чтение+запись** (`gm/bigfile.py`), проверено на 5 архивах; пересборка блобов даёт побайтово идентичные `.idx`/`.dat` |
| `.sub`/`.idx`-пары | `PICS/SBM.DAT`+`SBM.IDX` (текстуры), `sounds/SOU.DAT`+`SOU.IDX` | см. выше |
| `LOOK16.TAB`, `LOOK162.TAB` | кубы 64³ (262144 Б): «6-6-6 RGB → индекс палитры» (ближайший цвет, метрика `0x4228e0`). LOOK16 — по палитре игры (`0x5309b8` ← мастер `0x83d320`), LOOK162 — по палитре из `NEONS.PIN` (`0x530cb8`); значения LOOK162 = ровно 20 индексов NEONS.PIN. Генераторы `0x41e771`/`0x41e8ec`, CLI `gm.py look` | **разобран** |
| `.sog` | **текстовый плейлист** имён сэмплов; сами сэмплы — во втором архиве `MUSIC/SOU.DAT`+`SOU.IDX` (3092 записи, все `SOU2`) | **чтение+запись** (651 песня; `to_bytes` побайтово точен) |
| `.soa` | анимации: заголовок `<mode> <period> <frames> <loop>` + по имени кадра на строку | **чтение+запись** (104 файла; `to_bytes` побайтово точен) |
| `.bmp` | обычные Windows BMP (`PICS`, `FLAG`, `SML`, `sprites`) | прямые |

Подробные спецификации с полями — в [`docs/FORMATS.md`](docs/FORMATS.md).

### Звуковая система: Miles Sound System (AIL)

Загрузчик сэмплов в `gundx.exe` — по VMA `0x40af30` (магия проверяется по
`'S','O','U'` + цифра `'0'..'2'`); загрузчик музыки — `0x40ab20`.

* Секции `gundx.exe`: `adpcmseg`, `mixasmda`, `mixasmse`, `putseg`, `putsegda`,
  `tempscrs`; строки проб карт («SB 16 sound card found», «No sound card
  found»); ключи `/sb`, `/sbpro`, `/sb16`, `/sb32`, `/sb64`.
* Это **runtime-буферы**: таблицы шагов ADPCM грузились из AIL-segment файлов,
  которых в поставке нет. Поэтому сжатые `SOU1`/`SOU2` из установки **не
  декодируются** — только контейнер.
* Карта секций: `.text` file `0x600`→VMA `0x401000`; `.rdata` file `0x9fa00`→
  VMA `0x4a1000`; `.data` file `0xa1600`→VMA `0x4a3000` (отсюда VMA = fileoff −
  `0x5E600`); `adpcmseg` file `0xbe200`→VMA `0x842000` (VMA = fileoff +
  `0x783E00`).
* Строки звуковой подсистемы лежат в `.data`: `"sounds"` @ `0xa4e28`
  (VMA `0x4a6828`), `"Unable to load sample '%s' for song '%s'"` @ `0xa4e48`
  (VMA `0x4a6848`), `"%s.sog"` @ `0xa4e7e`, `"SOU"` @ `0xa4e9c`.
* Форматы по магии: `SOU0` → формат 0 (сырой 8-битный PCM, 13 лупов);
  `SOU1` → формат 3/2/1 при `rate <10000 / <18000 / иначе`; `SOU2` → формат 4
  (музыка). Структура записи сэмпла @ `0x83d960`, шаг 28; массив голосов
  @ `0x84e068`, шаг 0x50.
* Проверка «сжато, а не PCM»: `SILENCE` (SOU1, 8000 Гц, 400 байт) — точный
  3-байтовый цикл `4b 0c 20`, т.е. дельта-кодек держит константу. SOU0-записи
  гладкие (MAD1/MAD0 ≈ 0.1/0.33), SOU1/2 — почти равномерные (≈ 1.2).

### 2b. Видео, уровни, музыка — новые находки

* **`MUSIC/SOU.DAT` + `MUSIC/SOU.IDX`** — второй звуковой архив, 3092 записи,
  **все `SOU2`**. `sounds/SOU.IDX` — только эффекты/лупы (999 записей).
* **`.sog`** — текстовый плейлист имён сэмплов из этого архива. 25 песен
  ссылаются на 95 имён, которых нет ни в одном архиве (вырезанный контент).
* **`.sbw`** — разобран полностью (36/36 до EOF). После двух таблиц идёт
  `u32 count3` (число секторов), затем стадия 1: на каждый сектор `{u32 objects;
  u16 kind; u8 sub;` и на каждый объект `u32 id + 96-байтная шапка}`; затем
  стадия 2: на каждый объект `{u16 n_verts; u16 n_polys; n_verts × 3×f32;
  скаляры; n_polys × 0x80-байтовых записей}`. Заголовок объекта (96 Б) — origin
  + 3 базисных вектора, каждая компонента как `(u32 pad=0, f32)`; во всех
  28 192 объектах pad=0 и базис `diag(1.875)`. Вершины + индексы полигонов —
  это геометрия (индексы всегда в диапазоне); экспорт в OBJ через `--obj`.
  Декодеры: `0x42aa63` (таблицы), `0x42ad65` (стадия 1), `0x425f4c` (объект,
  вызывает `0x422b88`/`0x422a51` для аллокации вершин/полигонов).
  Поля объекта/полигона разобраны (полная таблица — `docs/FORMATS.md` §10.1):
  - объект: `radius` (0x60) = |`bounds`| (радиус описанной сферы), `bounds`
    (0x84) = полуразмеры AABB, `f74`/`f78`/`f7c` и 24-байтный хвост (0x90)
    всегда 0;
  - полигон (запись 0x80, `resolve_poly` `0x434463`): `texture` (0x00, знаковый
    1-based индекс: >0 текстурная таблица, <0 таблица эффектов, 0 нет;
    SET_POLY_TEXTURE), `material` (0x02, таблица ds:0x5670e0 → имена
    none/reflective/translucent/varadd/add/shade), `lightmap` (0x04, знаковый,
    0 = нет; пересчитывается базой лайтмапов уровня), шесть f32-троек
    0x08/0x14/0x20/0x2c/0x38/0x44 = центр, **нормаль**, **ось U**, **ось V**
    (последние три движок проверяет на единичность — `Invalid poly [U|V]
    vector`), и два опорных угла; `scroll_u/scroll_v` (0x5c/0x60, знаковые,
    SET_POLY_SCROLL; вместе с 0x5e/0x62/0x66 интерполируются для анимации
    скролла, `0x4342f0`), `brightness` (0x70, SET_POLY_BRIGHTNESS), `overlay`
    (0x78, индекс текстуры оверлея), `flags` (0x7c: база 0x8000, бит 0x2 =
    прозрачная), `reserved` (0x68) всегда 0. Диспетчер команд — jump-таблица
    `0x45c500` по индексу `id-1`; обработчики команд полигона: текстурa
    `0x457a40`, яркость `0x457a79`, скролл `0x457e7e`.
* **`.sbv`** — заголовок `SBV0`, `width<=320`, `height`, `frame_count` и таблица
  `{u32 timestamp_ms; u32 end_offset}` (смещение считается от
  `data_start = 16 + 8*frame_count`). Кадры: `Intro` 320×160×500,
  `orient` 4523, `winner` 1820. **Кодек разобран и реализован** (`gm/sbv.py`),
  побайтовая сверка с эмуляцией `gundx.exe` на 27 кадрах всех трёх файлов.
  Схема — собственная, чисто внутрикадровая **пирамидальная**:
  кадр = 3 плоскости (Y `W×H` + две цветности `W/2×H/2`, сэмплы `i16`).
  Плоскость = знаковые `i32 min`/`max`, затем байтовая сетка
  (Y `(W/4)×(H/4)`, цветность `(W/8)×(H/8)`), сэмпл =
  `(int)(byte*(max-min)/256.0 + min)` — деление на **256.0** (`0x4a2ac0`,
  усечение к нулю; прежние заметки про 500.0 были ошибочны).
  Остальное дочитывает **битовый** поток: блоки `factor×factor` (`0x49f4db`,
  4 → 8) + лифтинг/края/раздвижка (`0xa1d940`/`0x49f1e3`/`0x49f43a`).
  Энтропия (`0xa1d000`, MSB-first): унарный код класса (+знак, DPCM),
  класс `0` — блок нулей, `>=0x11` — конец плоскости; унарный код шага (+знак,
  DPCM), `step = 1<<(e&31)`; бит знака блока; статическая таблица обработчиков
  (`.data` `0xa1c400`). В реальных данных встречаются **только классы 0, 1 и 3**
  (полный прогон 6843 кадров: `{0: 6526044, 1: 3001203, 3: 2790153}`):
  класс 1 — `±step` c быстрым путём «4 нуля + cursor += 16»; класс 3 —
  `(1 + серия единиц)*step`, знак читается до серии. Классы 2/4..16
  («семейные», `0xa1d170+0x80k`) в данных отсутствуют и в самом движке
  выглядят несогласованными — реализованы best-effort (`FAMILY_BLOCKS`).
  YUV→RGB (`0x49e33b`): `r=(Y+C0+8)>>4`, `g=(Y-C0-C1+8)>>4`, `b=(Y+C1+8)>>4`,
  clamp 0..255; движок дополнительно прогоняет байты через три таблицы
  фиксированной точки (`0x4a6a40`/`0x4a6e40`/`0x4a7240`), вероятно гамма-рампу
  режима вывода.
* **`adpcmseg`** — секция в `gundx.exe` (`0x842000`, vsz `0x15540`) с говорящим
  именем, но её содержимое — данные/релокации (последовательности `xx xx 85 00`),
  не код; кодеки `.sou` (`SOU1`/`SOU2`) остаются в непоставляемых сегментах AIL.
* **`.soa`** — `<mode> <period> <frames> <loop>` + имена кадров (77 файлов).
* **`.sbt`** («World Things file») — **скомпилированный текстовый** файл мира.
  В движке есть текстовый загрузчик (`0x454b8d`) и писатель (`0x454d20`, `"wt"`);
  бинарный формат — это тот же текст после компиляции, а 1-байтовый тег записи
  равен id ключевого слова (`POLY_HANDLE`=15, `OBJECT_HANDLE`=16,
  `SECTOR_HANDLE`=17, `SCRIPT`=11, `END_OF_FILE`=255; строки форматов около
  `0x4b9600`, напр. `POLY_HANDLE` + `%ld %ld %ld %ld`). Заголовок 84 Б
  (`b"BINARY SAVE\0"` + нули до 80, затем `u32 version = 0`), затем записи:
  1 байт типа + нагрузка. `255` = `END_OF_FILE` (последний байт файла). Типы:
  `1 SWITCH` (`n*32`), `2 DOOR` (`n*184`), `3 THING` (`u8`+`0x528`), `4 THING_TYPES`
  (`n*936`: `char name[16]` + 49 `double` + 528 Б целочисленных полей; в движке
  грузятся из текстового `THING\<name>.soe`, `0x47cb6c`), `5 NAMED_ITEMS` (`n*52`,
  `char name[16]` + 9 `u32`; из текстового `item\<name>.soi`, `0x4689f0`),
  `6 SPRITES` (`n*44`), `7 SOUND_SOURCES`
  (`5n*4 + n*8`), `8 SECTOR_STUFF` (`0x5800`), `9 PLAYER_STATUS` (`u8`+`0x3510`),
  `11 SCRIPT` (загрузчик `0x462a80`: `u32 c` + `c*40` + `u32 n` + `n*4`×3 колонки
  + `u32 m` + `m` кода + `u8 flag` (+`9*0x400`); **ВМ разобрана, см. ниже**), `12 SOUND_SAMPLE_TABLE` (300 `u32` + 300 имён×9; все
  5800 ссылок находятся в `sounds/SOU.IDX`), `15 POLY_HANDLE`, `16 OBJECT_HANDLE`,
  `17 SECTOR_HANDLE`, `18 LIFT`, `19 ITEM` (`0x7080`),
  `20 SOB_TABLE` (`n*{char[12]; u32 amount; u32 ptr}`; имя → `SOBS/<name>.sob`
  или `Sobanim/<name>.soo`, третье поле — устаревший указатель). Типы 10/13 —
  заглушка -1. **Все 36 файлов разбираются ровно до EOF**, последовательность
  записей одинаковая: `12, 20, [15, 16,] 17, 5, 4, 11, 255`. Дизасм загрузчика:
  `0x454dc0` (бинарный) и `0x454b20` (текстовый вариант), диспетчер — таблицы
  `0x455604`/`0x455658`, имена типов — из строк ошибок обработчиков.
* **Таблицы handle 15/16/17 — по колонкам**: сначала `n` значений `tag`, потом
  `n` значений `sector` и т. д. (загрузчики читают каждую колонку в свой глобал:
  `0x5b21f8`, `0x618b88`, `0x5e5680`, `0x6183b8`). Колонки:
  `POLY_HANDLE = tag, sector, object, poly`; `OBJECT_HANDLE = tag, sector, object`;
  `SECTOR_HANDLE = tag, sector`. `(sector,object,poly)` — канонический адрес
  полигона (`resolve_poly` `0x434463`; отладочный оверлей печатает
  `SECTOR:%d OBJECT:%d POLY:%d` из глобалов `0x62cae0/e4/e8`). Сверено:
  **4277 poly / 207 object / 900 sector** handle из 36 уровней — все в диапазоне
  соответствующего `.sbw` (`gm.py sbt --resolve`). `tag` — непрозрачный id
  редактора: по нему адресуют команды `SET_HANDLEPOLY_*`/`SET_HANDLESECTOR_*`
  (сигнатуры `LLLL`/`LLLLL`), тогда как `SET_POLY_*` адресует прямо тройкой
  `(sector,object,poly)`.

* **`SCRIPT` (11) — скриптовая ВМ (разобрана полностью).** Блок кода =
  поток `[u8 опкод][операнды]`, опкод = id команды, `0xFF` — конец
  под-скрипта. Размер операндов — по сигнатуре (функция `0x457170`, таблица
  букв `0x457204`): `A/B/D`=8 (double), `Y`=1, `S`=строка с NUL, прочие
  (`E G I L M N O P R T V Z`)=4. Две таблицы команд (обе вшиты в
  `gm/sbtvm.py`): `COMMANDS` (`0x4b53e8`, 192 именованные команды редактора;
  диспетчер `0x45c500` по `id-1`) и `CONDITIONS` (`0x4b62ec`, 59 команд
  второй ВМ условий/выражений; обработчик `0x45f490`, диспетч `0x45f8e0`).
  Безымянные *структурные* id — на самом деле **именованные**: имена сняты с
  текстового загрузчика `0x460eca`, который матчит токены `DEFINE_WEAPONS`,
  `DEFINE_AMMO`, `DEFINE_EQUIPMENT`, `DEFINE_PLAYER_TYPES`,
  `DEFINE_LEVEL_OBJECTIVES`, `ADD_LIFT`, `SOURCEPLAYER_PATH`, `IF`, `ELSE`,
  `END`, а затем эмитит опкод + счётчик + операнды записи обходчиком
  `0x461bc0` по пулированной сигнатуре:

  | id | имя | раскладка |
  |---|---|---|
  | 103 | `DEFINE_LEVEL_OBJECTIVES` | `u32 n≤30` + `n` строк |
  | 158 | `SOURCEPLAYER_PATH` | `u32 n≤32` + `n*4` |
  | 170 | `DEFINE_WEAPONS` | `u32 n≤31` + `n*{3 строки + 0x54}` |
  | 171 | `DEFINE_AMMO` | `u32 n≤31` + `n*{1 строка + 0x28}` |
  | 172 | `DEFINE_PLAYER_TYPES` | `u32 n` + `n*{3 строки + 0x17c}` |
  | 175 | `DEFINE_EQUIPMENT` | `u32 n≤31` + `n*{1 строка + 0x30}` |
  | 188 | `ADD_LIFT` | `0x94` + `u32 n≤31` + `n*0xc` |
  | 199 | `IF` | `u8 cond` + операнды + `u32 size` + тело |
  | 200 | `ELSE` | `u32 rel` (прыжок мимо else-ветки) |
  | `0xC9` | `EVAL` | `u8 cond` + операнды `CONDITIONS[cond]` |

  Размеры хвостов побайтово подтверждены сигнатурами
  (`SLEOLLLLLLL`=0x28, `SLEEEEEELLLLL`=0x30, `SSSLLLLEEEELLLLDBBDL`=0x54,
  `SLEO..`/`SSSLDLLD…EE`=0x17c, `LOLLLLLL…VNVNVN`=0x94, `ELE`=0xc, `E`=4).
  Имена таблиц подтверждены ошибками exe (`Maximum number of
  ammo/weapon/equipment types exceeded`). Прим.: диспетчер (`0x457390`)
  держит указатель кода в `[0x4b6784]`, регистр-курсор `ebx`, лимит
  `0x30000`. Декодер `gm.sbtvm.decode/render`: **36/36 до байтового конца**
  (`gm.py sbt --script`).

### Известные имена ресурсов (из строк `gun.exe`)

```
%s.SBT  %s.SBW  %s.sob  %s.soe  %s.sbm  %s.sog  %s.sof
savegame\saved%d.sbt / saved.sbc / saved.sbw / saved.sow / saved.sot
SML\BRIEF\LEVEL%ld.SML     SML\PRELEVEL\%s.SML   SML\DESIGN\%s%ld.SML
FRONTEND\killed.skk / .skb   FILM\%s.fdx   FILM\%s.sof
SPRITES\%s.sbm   THING\%s.soe   ITEM\%s.soi   SOUNDS\sounds.sub
look16.tab   look162.tab   neons.pin
```
Внутренние режимы отладки движка: `GOD SECTOR MODE`, `GOD POLY LIGHTMAP MODE`,
`GENERATE WHOLE WORLD LIGHTMAPS`, `SET_SECTOR_*`, `SET_HANDLESECTOR_*`.

## 3. Уровни

36 комплектов `levelNN.sbc` + `.sbt` + `.sbw` (+ `dm1..dm4`, `secrt1/2`, `temple`).
Имена — в `lvlnames.slu` (LEVEL1 Testing Range … DM4 Deathmatch Till Dawn).
Лог `GUNMETAL.LOG` подтверждает структуру мира:
`sector[34].object[11].poly[4]`, бюджеты `Texture mem` / `Level mem` /
`Lightmap mem` / `Lookup table mem`.

## 4. Что уже сделано сообществом

- `github.com/dingusdev/gunmetal-decomp` — декомпиляция на C (11 коммитов,
  10 файлов: `game.c`, `glide.c`, `film.c`, `doors.c`, `script.c`, `input.c`,
  `log.c`, `ddsupport.c`, `safememc.s`). **Репозиторий сейчас удалён/закрыт (404)**,
  последний снимок в Wayback — 23.03.2026. Релизов не было.
- Тред VOGONS `t=73243` — «Attempting to find out how Gunmetal (1998) works via DOSBox».

## 5. Инструментарий в среде

- Есть: `python3` 3.14, `objdump`, `readelf`, `nm`, `strings`, `xxd`, `curl`.
- Нет: Ghidra, radare2/rizin, Java, wine, `pip`/`numpy`/`PIL`/`capstone`/`pefile`,
  `7z`, `unrar`, `sudo` без пароля.
- Что поставить без root (в `~/tools`): OpenJDK + Ghidra, либо статический
  rizin/radare2. Python-зависимости придётся тащить как vendor-каталоги (`curl`).

## 6. Тулчейн `gm.py`

Чистый Python 3 (только стандартная библиотека; PNG/BMP написаны вручную в
`gm/png.py`, `gm/bmp.py`, палитра — `gm/palette.py`). Подкоманды:

| Команда | Что делает |
|---|---|
| `list <idx>` | таблица записей архива BIGFILE |
| `extract <idx> <out>` | сырые блобы архива |
| `pack <out.idx> <dir> [--ext .EXT]` | собрать BIGFILE `.idx`/`.dat` из каталога (имя = стем файла) |
| `set <idx> <name> <file> <out.idx>` | заменить/добавить одну запись архива |
| `sbm <image> <out.sbm> [--palette BMP] [--game DIR] [--mips N] [--flags N] [--filter box\|nearest]` | закодировать PNG/BMP в `.sbm` |
| `textures <game> <out>` | `PICS/SBM.DAT` → PNG (общая палитра) |
| `sprites <game> <out>` | `sprites/SBM.DAT` → PNG (индекс 0 прозрачен) |
| `sounds <game> <out>` | `sounds/SOU.DAT`: `SOU0` → WAV, `SOU1/2` → сырые дампы |
| `frontend <game> <out>` | `FRONTEND/BMP.DAT` → BMP |
| `sob <src> <out>` | `.sob` → OBJ + MTL |
| `soa <paths...> [--rebuild DIR]` | список анимаций `.soa`; `--rebuild` — запись обратно |
| `soe <paths...> [-v] [--rebuild DIR]` | разбор `.soe` (текстовые «thing»-определения); `--rebuild` — запись обратно |
| `look FILE [--png OUT] [--palette BMP] [--neons PIN] [--scale N]` | анализ кубов `LOOK16.TAB`/`LOOK162.TAB`; `--png` — монтаж 8×8 |
| `sog <path> [--music IDX] [--rebuild DIR]` | разбор песен `.sog` (плейлисты); `--rebuild` — запись обратно |
| `sbv <file> [out] [--png DIR] [--frames SPEC] [--scale N] [--no-raw]` | `.sbv`: заголовок/таблица, сырые блоки кадров и/или декодированные PNG |
| `sbw <files...> [-v] [--obj DIR] [--transform] [--rebuild DIR]` | мир `.sbw`: таблицы + секторы/объекты/геометрия, экспорт в OBJ, `--rebuild` — запись обратно (побайтово) |
| `sbt <paths...> [-v] [--resolve] [--json DIR] [--rebuild DIR] [--script] [--script-out DIR]` | записи `.sbt` (мир/уровень); `--resolve` сверяет handle с соседним `.sbw`, `--rebuild` — запись обратно (побайтово), `--script` — декомпиляция скриптового блока |
| `sbc <paths...> <out> [--rebuild DIR]` | `.sbc` → градационные PNG + сводный лист; `--rebuild` — запись обратно |

Установка (`D:\games\Gunmetal 3dfx`) **не изменяется**: все результаты идут в
`~/gunmetal-re/` и `/tmp/opencode/`.
