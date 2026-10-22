# Поток кампании: счётчик миссий, штаб, цели, секреты, время, финал (`M9.1`)

* **Статус:** карта снята по `gundx.exe` v1.36 (ImageBase `0x400000`) и по
  поставочным `.sbt`/`SAVEGAME` — игра только на чтение. Ветка
  `feat/campaign`, база `master` `ae100c8`.
* **Инструмент:** `tools/re/campaign_flow.py` (новый; `tools/re/campaign.py`
  не менялся, а импортируется как модуль). Проверки: `--self-test` — **35/35**,
  `--summary` — 36 уровней + цепочка 24 миссий, `--json` — 36/36, `--fields` —
  35 записей карты полей.
* **Канон:** формат/поведение — `docs/FORMATS.md` §15, поля сейва —
  `docs/SAVE_FIELDS.md`, загрузка уровня — `docs/LEVEL_LOAD.md`, опкоды —
  `docs/VM_EXEC.md`, события — `docs/EVENTS.md`, экипировка —
  `docs/EQUIPMENT.md` §5. Здесь — только поток кампании и то, что в тех доках
  не было.

Обозначения: **код** — адрес/инструкция в exe или байты скрипта; **данные** —
измерено на поставке (N/N); **гипотеза** — не доказано.

## 1. Схема

```
фронтенд "NEW GAME"  0x4428f0(0) / 0x442a0e
   имя LEVEL0, counter = 1                     docs/FORMATS.md 15
        |
        v
level0 (штаб, стартовая сборка)                58 секторов, 192 объекта
   предмет с E=305 (ADD_ITEM_FIXED, 0xc7e3) -> entity 305
        |                                      entity 305: IF SOURCE_IS_PLAYER NEXT_LEVEL
        v                                      NEXT_LEVEL: sprintf("LEVEL%ld", counter); counter++
level1 ... level8 (миссии, акт 1)
   цели/секреты/время -> SHOW_LEVEL_END_SCREEN (entity 305)
        | 0x83cb7c = 5  ->  engine posts SYSTEM_LEVEL_FINISHED (entity -8)  0x4454a2
        v
entity -8: NEW_LEVEL "level00"                  счётчик не меняется
        |
        v
level00 (штаб акта 1): entity 305 -> NEXT_LEVEL -> LEVEL<counter> (= следующая миссия)
        ...
level9 ... level16 (акт 2) -> level000
level17, level18 (акт 3)   -> level0a
level19                    -> NEXT_LEVEL напрямую (без штаба)
level20 ... level23 (акт 4) -> level0b (COMMAND POST)
level24                    -> в кампании выхода нет (только DM-ветка `dm1`)
```

Один и тот же механизм «штаб/миссия»:

| кто | entity `305` | что делает |
|---|---|---|
| штаб (`level0`, `level00`, `level000`, `level0a`, `level0b`) | `0xc86e`, `0xcb08`, `0xc80a`, `0xbfcf`, `0xbd3b` | `IF SOURCE_IS_PLAYER { NEXT_LEVEL }` |
| миссия | напр. `level1` `0xd15c`, `level19` `0xf584`, `level24` `0xf891` | `IF SOURCE_IS_PLAYER { SHOW_LEVEL_END_SCREEN }` |

То есть «штаб» — обычный уровень с магазином, у которого нет целей и нет
`entity -8`, а выход в миссию сделан `NEXT_LEVEL` (код: `0x457b11`).
`level0` — та же база, но стартовая сборка: её запускает фронтенд новой игры
(`0x442a0e` ставит имя `LEVEL0` и `counter = 1`), своих `-8`/`305`-миссий у неё
нет, а `305` ведёт в первую миссию.

## 2. Счётчик миссий

**Поле:** `ds:0x80f9cc`, u32 — **номер следующей миссии** (не «текущей»).

Доказательство семантики: `NEXT_LEVEL` (`cid` 0x3e, обработчик `0x457b11`)
делает ровно

```
sprintf(ds:0x5a47c8, "LEVEL%ld", ds:0x80f9cc)   0x457b17..0x457b21 (fmt 0x4b8588)
ds:0x80f9cc++                                   0x457b26..0x457b2f
```

а условие `NEXT_LEVEL_NUMBER n` (`cid` 8, `0x45ea26`) возвращает
`ds:0x80f9cc == n` (**код**).

| кто | адрес | что делает |
|---|---|---|
| новая игра (фронтенд) | `0x442a0e`, `0x442a4c`, `0x442cf7` | `= 1` |
| новая игра, «пустой» уровень | `0x442a39`…`0x442a4c` | имя `LEVEL0` из `0x4ab6d4`, `= 1` |
| `NEXT_LEVEL` | `0x457b2e` | `+= 1` после сборки имени |
| «Restart level from HQ» | `0x44295e`, кламп `0x442973` | `-= 1`, минимум 1 |
| загрузка сейва | `0x45586e` | читает F2 как есть |
| сохранение | `0x454f19` | пишет F2 (сразу после часов `0x5a46e4`) |
| брифинг | `0x48f0f3` (counter), `0x48f116`/`0x48f13e` (counter−1) | `SML\BRIEF\LEVEL%ld.SML` |

**Поправка к `docs/FORMATS.md` §15.** Там написано «при загрузке сейва
уменьшается на 1 (`0x44295e`)». По коду декремент стоит не в загрузке сейва, а
в `0x4428f0(param)` — входе «рестарт уровня» (`docs/LEVEL_LOAD.md` §2), причём
только в ветке `param & 1` и только если `strnicmp(0x5a48b8,"LEVEL0",6) != 0`
(`0x442952`); кламп `>= 1` — `0x442973`. Ветку `param=1` вызывает пункт меню
паузы **«Restart level from HQ»** (`0x4bd3fc`; `0x488931` кладёт id в
`0x4bc280`, фронтенд `0x48da09` → `0x4428f0(1)`), который прячется для
`LEVEL20`/`SECRT1`/`SECRT2` (`0x4888e4`). Сейв грузится другим входом
(`0x442340`, `docs/LEVEL_LOAD.md` §2) и счётчик не трогает. Смысл декремента:
миссия загружена при `counter = N+1`; «рестарт из штаба» возвращает `N`, чтобы
штаб снова загрузил ту же миссию.

Следствия (проверены инструментом на всех 24 миссиях, 0 расхождений):

* пока играешь миссию *N*, `counter = N+1`;
* `level19` — единственная миссия без штаба: её `-8` делает `NEXT_LEVEL`
  (`0xf5a0`) при `counter = 20` и сразу грузит `LEVEL20`;
* у `level24` в кампании выхода нет (см. §7), `counter` остаётся 25.

## 3. Как выбирается следующий уровень

Два обработчика, оба в таблице `COMMANDS` (`docs/FORMATS.md` §15):

| команда | cid | обработчик | эффект |
|---|---:|---|---|
| `NEW_LEVEL "stem"` | 0x3d | `0x457af3` | `strncpy(0x5a47c8, stem, 0x4f)`, NUL в `0x5a4817`; счётчик не меняется |
| `NEXT_LEVEL` | 0x3e | `0x457b11` | имя из счётчика + инкремент |
| `END_GAME` | 0x3f | `0x45758f` | `0x44b830` + флаг выхода; в поставке **0 сайтов** |

Дальше `0x442ef0` (`load_level`) переносит `0x5a47c8 → 0x5a4728`, грузит
`<stem>.sbw/.sbt/.sbc` и в конце ставит `0x83cb7c = 0x83d300`
(`docs/LEVEL_LOAD.md` §3).

**Штабы и акты.** Разбивка на акты зашита в скрипты миссий: миссия возвращает
игрока в *свой* штаб по имени, а штаб вызывает `NEXT_LEVEL` (номер миссии
берётся из счётчика, а не из штаба). Измерено (`--summary`, 24/24):

| акт | миссии | штаб | почему |
|---|---|---|---|
| старт | — | `level0` | новая игра, `0x442a0e` |
| 1 | `level1`…`level8` | `level00` | `NEW_LEVEL "level00"` в каждой |
| 2 | `level9`…`level16` | `level000` | `NEW_LEVEL "level000"` |
| 3 | `level17`, `level18` | `level0a` | `NEW_LEVEL "level0a"` |
| 4 | `level20`…`level23` | `level0b` | `NEW_LEVEL "level0b"` |
| — | `level19` | нет | `NEXT_LEVEL` напрямую (`0xf5a0`) |
| — | `level24` | нет | в кампании выхода нет |

`level6` — исключение: у него два под-скрипта `-8`
(`0xdac6` и `0xdbef`), поэтому в кампании допустимы **оба** штаба —
`level00` (`0xdad9`) и `level0a` (`0xdc02`); на номер следующей миссии это не
влияет (его задаёт счётчик). `level4` и `level16` имеют секретные ветки
(`secrt1` `0xde21`, `secrt2` `0xe44d`) — они не меняют номер миссии.

Пять штабов — это магазины/армори (`docs/EQUIPMENT.md` §5):

| штаб | имя | `ADD_SWITCH`/`ADD_DOOR` | buy-экраны (ammo/item/tank buy/tank save/weapon) | `DEFINE_WEAPONS`/`AMMO`/`EQUIPMENT` |
|---|---|---:|---|---|
| `level0` | HEADQUARTERS | 24 / 25 | 1/1/1/1/1 | 2/2/1 |
| `level00` | HEADQUARTERS | 21 / 24 | 1/1/1/1/1 | 2/2/1 |
| `level000` | HEADQUARTERS | 21 / 24 | 1/1/1/1/1 | 2/1/1 |
| `level0a` | HEADQUARTERS | 21 / 24 | 1/1/1/1/1 | 1/1/1 |
| `level0b` | COMMAND POST | 8 / 0 | 1/1/1/1/1 | 1/1/1 |

Счётчики `ADD_SWITCH`/`ADD_DOOR` совпадают с `docs/FORMATS.md` §15 (24/25,
21×3, 8/0). Штаб также показывает статистику «SECRETS FOUND» (`0x499534`,
строки `0x4beb10` и формат `%ld/%ld` `0x4be7d8`) и открывает брифинг
`SHOW_BRIEFING_SCREEN` (`0x45c23e`, по одному на штаб).

Цепочка, выведенная из счётчика (`--summary`, таблица ниже):

| # | файл | counter во время миссии | возврат (-8) | следующий грузится |
|---:|---|---:|---|---|
| 1 | `level1` | 2 | `level00` | `LEVEL2` |
| 2 | `level2` | 3 | `level00` | `LEVEL3` |
| 3 | `level3` | 4 | `level00` | `LEVEL4` |
| 4 | `level4` | 5 | `level00` | `LEVEL5` |
| 5 | `level5` | 6 | `level00` | `LEVEL6` |
| 6 | `level6` | 7 | `level00` и `level0a` | `LEVEL7` |
| 7 | `level7` | 8 | `level00` | `LEVEL8` |
| 8 | `level8` | 9 | `level00` | `LEVEL9` |
| 9 | `level9` | 10 | `level000` | `LEVEL10` |
| 10 | `level10` | 11 | `level000` | `LEVEL11` |
| 11 | `level11` | 12 | `level000` | `LEVEL12` |
| 12 | `level12` | 13 | `level000` | `LEVEL13` |
| 13 | `level13` | 14 | `level000` | `LEVEL14` |
| 14 | `level14` | 15 | `level000` | `LEVEL15` |
| 15 | `level15` | 16 | `level000` | `LEVEL16` |
| 16 | `level16` | 17 | `level000` | `LEVEL17` |
| 17 | `level17` | 18 | `level0a` | `LEVEL18` |
| 18 | `level18` | 19 | `level0a` | `LEVEL19` |
| 19 | `level19` | 20 | `NEXT_LEVEL` (сама) | `LEVEL20` |
| 20 | `level20` | 21 | `level0b` | `LEVEL21` |
| 21 | `level21` | 22 | `level0b` | `LEVEL22` |
| 22 | `level22` | 23 | `level0b` | `LEVEL23` |
| 23 | `level23` | 24 | `level0b` | `LEVEL24` |
| 24 | `level24` | 25 | — | конец |

Инструмент проверяет инвариант: каждая из 24 миссий встречается ровно один раз,
у каждой (кроме 24) есть ровно один шаг счётчика, пропусков нет
(`--summary` печатает `ok: 24 missions ...`).

**Независимая сверка.** `--summary` считает все сайты переходов по 36 уровням:
**66** = 60 `NEW_LEVEL` + 6 `NEXT_LEVEL`, ровно как в `docs/VM_SCENE.md` §4
(там 66 сайтов исполнены ВМ). Совпали и 6 `NEXT_LEVEL`: пять штабов +
`level19`.

## 4. Цели

| что | адрес | доказательство |
|---|---|---|
| `DEFINE_LEVEL_OBJECTIVES` (103) | обработчик `0x45bdf0` | `cmp eax,0x1e` — максимум 30 (`0x45bdf6`); count → `0x80d7ec` (`0x45be04`) |
| текст целей | `0x80d800 + 4*i` | указатель на строку кода: `0x45be11`/`0x45be21`, шаг `0x45be34` |
| биты выполнения | `0x80f9d0` | `OR_LEVEL_OBJECTIVES_BITS` (104) `0x45be40`, `or` в `0x45be4e`; сброс `0x4570a3` |
| проверка завершения | условие `LEVEL_OBJECTIVES_COMPLETE` (cid 7), `0x45e9f0` | цикл `i < 0x80d7ec`, все `1<<i` в `0x80f9d0`; `count<=0` → ложь |
| что видит игрок | `FUN_004869c0` (`0x487337`) | рисует строки `0x80d800[i]` и биты `0x80f9d0` (`0x4833e0`); вызывается `0x487aec`, `0x488309` |

Измерено на 36 уровнях (`--summary`/`--json`):

* 25 уровней объявляют цели, всего **51** строка; **35** объявлений
  (у `level2`, `level7`, `level18`, `level19` — по несколько вариантов под
  разными `IF`, поэтому в таблице `obj` показан как `N(variants)`);
* **19** уровней реально ставят биты (`OR_LEVEL_OBJECTIVES_BITS`), и ровно на
  этих же 19 уровнях есть проверка `LEVEL_OBJECTIVES_COMPLETE` (48 сайтов);
* **6** уровней объявляют цели, но не ставят биты: `dm3`, `level13`,
  `level16`, `level18`, `level21`, `level24`. Для них цель — только текст на
  экране, а конец миссии запускается другим триггером (у `level24` — подрыв
  ядра, §7).

**Сейв.** Строки целей остаются в блоке 11 (это код уровня), а биты
дописываются в конец блока как команда `OR_LEVEL_OBJECTIVES_BITS`:

* писатель сейва `0x455bcf` вызывает `FUN_00462db0`; тот аллоцирует сущность
  с id **`0xfffffffd`** (`-3`, `INTERNAL_LEVEL_RESUME`, `docs/EVENTS.md` §4.1)
  и её смещением делает текущий курсор кода (`0x462dc9`…`0x462e35`;
  id — `0x462e0c`, смещение — `0x462e19`…`0x462e20`);
* если `0x80f9d0 != 0`, в этот под-скрипт пишется опкод `0x68` = 104
  (`OR_LEVEL_OBJECTIVES_BITS`) и значение (`0x462f2a`…`0x462f60`); так же
  дописываются музыка (`0x52`), туман (`0x62`) и таймер (`0x63`);
* проверено на `SAVEGAME/saved0.sbt`: блок 11 = код `level0` (62 497 байт)
  + под-скрипт `-3` в 6 байт `52 06 00 00 00 ff` (`PLAY_MUSIC 6; END`);
  в таблице сущностей сейва есть строка `id=-3, offset=62497`.

То есть `objectives_bits` **хранятся в сейве** (блок 11, через `-3`), а не в
отдельном поле шапки. Живого сейва с выполненными целями в поставке нет
(оба сейва — `level0`), поэтому end-to-end проверка не сделана.

## 5. Секреты

| что | адрес | доказательство |
|---|---|---|
| объявлено секретов в уровне | `SET_LEVEL_SECRETS` (94) → `0x80d8c0` | `0x45b816..0x45b825`; сброс `0x45708a` |
| найдено (на игрока) | `player+0x33cc` (`0x81c40c`) | `INCREMENT_SOURCEPLAYER_SECRET_TALLY` (95) `0x45b978` → `0x4536b0`: `*(int*)(0x81c40c + i*0x3510) += 1`, индекс игрока 0..7 |
| сброс при загрузке уровня | `0x45280f` | внутри `0x452730`, вызывается из `0x452430` (`0x4526ab`) при старте уровня (`0x442f5a`) |
| что видит игрок | `0x483c07` (экран конца уровня), `0x499534` (панель) | формат `%ld / %ld` `0x4bcb78`, подпись `SECRETS FOUND:` `0x4bcbb0` / `0x4beb10` |

Порядок вывода на экране конца уровня: сначала найденные (tally), потом
объявленные (`0x80d8c0`) — `0x483c07` кладёт `0x80d8c0` вторым аргументом,
`0x442290` читает tally.

**Сейв.** tally лежит в блоке 9 (структура игрока `0x3510`, смещение `+0x33cc`
входит в неё), объявленное число — статическое поле уровня (`SET_LEVEL_SECRETS`
в коде, блок 11). Так как tally обнуляется при каждой загрузке уровня, в сейве
оно имеет смысл только для сейва «внутри уровня».

Измерено (`--summary`), статические сайты `INCREMENT_...`:

| уровень | объявлено | сайтов инкремента | примечание |
|---|---:|---:|---|
| `level3` | 6 | 8 | два сайта — альтернативные ветки |
| `level15` | 5 | 4 | на один секрет меньше, чем объявлено |
| `secrt2` | — (нет `SET_LEVEL_SECRETS`) | 2 | счётчик может превысить «объявлено» |
| `temple` | — | 1 | то же |
| остальные 21 | 1…6 | столько же | — |

Всего по поставке: 101 «объявленный» секрет и 105 сайтов инкремента.

**Поправка к `docs/PLAYER_FIELDS.md`.** Поле `player+0x33cc` там названо
`damage_accum` / unknown. Это счётчик найденных секретов: его инкрементирует
только `0x4536b0` (из `0x45b978`), обнуляет старт уровня (`0x45280f`), а
читают только два места, рисующие «SECRETS FOUND» (`0x49953a`, `0x483c07`).

## 6. Лучшее время

Есть **две разные величины**, и их легко перепутать:

| величина | где | кто пишет | кто читает |
|---|---|---|---|
| «par» уровня | `0x80d88c` | `SET_LEVEL_BEST_TIME` (93), `0x45b82a..0x45b835` | экран конца уровня: `BEST TIME:` (`0x483caf`) |
| рекорд игрока | файл `<game>/GUNMETAL.HI` | `0x45c880` (вставка при улучшении) | тот же `0x45c880`; на экране `YOUR BEST:` (`0x483ce2` из `0x83d2c0`) |
| время текущего прохождения | `0x83d638` | `0x45c057`: `clock 0x5a46e4 / 91` | `TIME:` (`0x483c7c`) |

`GUNMETAL.HI` (файл есть в поставке, **576 байт** = 9 записей):

```
запись 0x40 байт:  i32 seconds;  char name[60]   (имя — SET_LEVEL_NAME, не ствол)
```

Имя для сравнения берётся из буфера `0x80f860`, куда `SET_LEVEL_NAME`
(`0x45b73c`, копия `0x45b753`) кладёт строку уровня; поэтому в файле
`TESTING RANGE`, `KELLER CORE`, а не `level1`/`level24`.

* открывается через `0x493630` (возвращает указатель на строку `GUNMETAL.HI`
  из `0x4be60c`);
  режим `"rb"` (`0x4a6388`) для чтения и `"ab"` (`0x4b8690`) для дописывания;
* чтение — `fread(buf, 0x40, 1, f)` (`0x40dc70`), имя сравнивается
  регистронезависимо (`0x416850`), минимум времени по имени — `0x45c8d6`;
* если новое время меньше минимума, запись дописывается (`0x45c904..0x45c94c`)
  и функция возвращает новое время; иначе возвращает найденный минимум;
  результат кладётся в `0x83d2c0` (`0x45c08e`);
* вызывающих два: экран конца уровня `0x45c086` и чит `0x460569`
  (`MadCastleArrgh`, `0x4aa364`, сравнение `0x46052c`).

Файл в поставке (данные): 146 `TESTING RANGE`, 1570 `KELLER CORE`,
18 `TESTING RANGE` (дубликат имени, минимум = 18), 109 `POWER STATION`,
18/7 `NATAKA TOWER`, 11 `OIL RIG`, 40 `OIL REFINERY`, 6 `CADTLELTL`.
Дубликаты имён законны: вставка не удаляет старое имя, только дописывает.
Сравнение par и рекорда показывает, что par — не рекорд, а «эталон» уровня:
`TESTING RANGE` par 48 при рекорде 18, `KELLER CORE` par 260 при 1570.

`SET_LEVEL_BEST_TIME` и `SET_LEVEL_SECRETS` **не являются состоянием**:
писатель сейва (`0x462db0`) дописывает только музыку/туман/таймер/биты целей,
а в блоке 11 остаётся код уровня (там эти две команды уже есть) — при
возобновлении они не «восстанавливаются», а исполняются из кода.

## 7. Финал и титры

* `END_GAME` в поставке **не встречается** (0 сайтов; `docs/VM_SCENE.md` §4) —
  кампания заканчивается не командой, а отсутствием перехода.
* `level24` (`KELLER CORE`): `-8` (`0xf799`) = `IF DEATHMATCH_GAME { NEW_LEVEL "DM1" }`
  и больше ничего (`0xf7a4` — `ELSE`, за ним `EVAL; END`). В кампании выхода
  нет; после подрыва ядра `entity -30308` показывает
  `MISSION OBJECTIVES COMPLETE` и генерирует `305` → экран конца уровня
  (`0xf849`, `0xf891`). Дальше играть некуда — остаёшься на экране результатов.
* **Титры** — отдельный путь фронтенда, не автоматический ролик после
  `level24`:

| шаг | адрес | что |
|---|---|---|
| пункт меню «credits» | `0x4971e0`, `jmp [eax*4+0x4972cc]`, индекс 3 → `0x4972c1` | обработчик действия; при `0x83d940 == 0` (`0x497264`) |
| запуск | `0x483470` | `0x4913a0("winner")` → `Vid/winner.sbv`; `0x4bc440 = 1`; `0x44b830` (стоп уровня) |
| видео | `0x4913a0` → `0x49e1e8("vid", name)` | расширение `sbv` — строка `0x4bee28`; ошибки `0x4be51c`/`0x4be534` |
| титры | `0x497310("SML\\CREDITS.SML")` (`0x4bd228`) | пейджер SML (`docs/UI.md` §6.3), буфер `0x22600` байт (`0x4834c1`), ошибка `0x4bcaac` |
| гейт | `0x83d940` | `(0x83d63c == 2)`, ставится `0x442e73..0x442e9e`; при 1 пункт не срабатывает |

* `FINALE`/`OVERTURE`/`WINNER` — **не** команды и не `.sof`-фильмы: это три
  стерео-SOU2 сэмпла в `sounds/SOU.IDX` (`docs/SOUND_FIELDS.md` §7,
  `docs/SOU_CONTAINER.md` §1). Ни в одном из 36 скриптов нет ссылки на
  `FINALE`/`OVERTURE`, и в exe нет таких строк — «финал через FINALE» не
  подтверждается.

**Гипотеза:** автоматического показа титров после `level24` нет; титры
доступны только из меню (и, возможно, включаются фронтендом по какому-то
признаку прохождения — признак не найден: `0x80f9cc` никто не сравнивает с
25/24, кроме `NEXT_LEVEL_NUMBER`).

## 8. Итоговая таблица по 36 уровням

Полный вывод `--summary` (36 строк; колонки: `obj` — число целей основного
`DEFINE_LEVEL_OBJECTIVES`, в скобках — сколько вариантов объявлено; `bits` —
объединение всех `OR_LEVEL_OBJECTIVES_BITS`; `tly` — сайтов
`INCREMENT_SOURCEPLAYER_SECRET_TALLY`; `secr` — `SET_LEVEL_SECRETS`; `par` —
`SET_LEVEL_BEST_TIME`; `fin/exit` — смещение `entity -8` и `entity 305`
(`xN` — у уровня N под-скриптов `-8`; показан первый),
`+Ngen` — `-8` состоит из `GENERATE` на N под-скриптов):

```
level     role   name                         obj   bits   tly  secr   par yourbest  fin/exit  exits
level0    hq     HEADQUARTERS                   0      -     0     -     -       -  -/0xc86e  NEXT_LEVEL [SOURCE_IS_PLAYER]
level00   hq     HEADQUARTERS                   0      -     0     -     -       -  -/0xcb08  NEXT_LEVEL [SOURCE_IS_PLAYER]
level000  hq     HEADQUARTERS                   0      -     0     -     -       -  -/0xc80a  NEXT_LEVEL [SOURCE_IS_PLAYER]
level0a   hq     HEADQUARTERS                   0      -     0     -     -       -  -/0xbfcf  NEXT_LEVEL [SOURCE_IS_PLAYER]
level0b   hq     COMMAND POST                   0      -     0     -     -       -  -/0xbd3b  NEXT_LEVEL [SOURCE_IS_PLAYER]
level1    mission TESTING RANGE                  4    0xf     4     4    48      18 0xd047/0xd15c  level2 [DEATHMATCH_GAME]; level00
level10   mission SPACEPORT                      1    0x1     3     3    69       - 0xcfb9/0xd0d5  level11 [DEATHMATCH_GAME]; level000
level11   mission ISLAND FORTRESS                2    0x3     1     1   289       - 0xd736/0xd857  level12 [DEATHMATCH_GAME]; level000
level12   mission SPACE STATION                  2    0x3     6     6   561       - 0xe6bd/0xe885  level13 [DEATHMATCH_GAME]; level000
level13   mission MINING ASTEROID                1      -     3     3   273       - 0xdc73/0xdc92  level14 [DEATHMATCH_GAME]; level000
level14   mission DIVINE LOGIC                   1    0x1     3     3   258       - 0xeb01/0xec1d  level15 [DEATHMATCH_GAME]; level000
level15   mission SPARTA                         5   0x1f     4     5   157       - 0xf405/0xf3f8  level16 [DEATHMATCH_GAME]; level000
level16   mission CULT MOON                      1      -     5     5   286       - 0xe423 +2gen/0xe396  level17 [DEATHMATCH_GAME]; level000; secrt2
level17   mission WEAPONS FACILITY               3    0x7     3     3   609       - 0xf102/0xf07f  level19 [DEATHMATCH_GAME]; level0a
level18   mission NATAKA ARCOLOGY             1(2)      -     6     6   231       - 0xe48e/0xe486  level19 [DEATHMATCH_GAME]; level0a
level19   mission WASTELAND                   2(7)    0xf     2     2   400       - 0xf58c/0xf584  level20 [DEATHMATCH_GAME]; NEXT_LEVEL
level2    mission POWER STATION               2(3)    0x3     5     5   180     109 0xd4d3/0xdaa3  level3 [DEATHMATCH_GAME]; level00
level20   mission BASE ATTACK                    4    0xf     3     3   204       - 0xdce4/0xd9d2  level21 [DEATHMATCH_GAME]; level0b
level21   mission THE RUINS                      1      -     6     6   452       - 0xf277/0xf26f  level22 [DEATHMATCH_GAME]; level0b
level22   mission TELEPORT DEPOT                 2    0x3     4     4   412       - 0xf358/0xf46f  level23 [DEATHMATCH_GAME]; level0b
level23   mission TANK FACTORY                   2    0x3     3     3   462       - 0xf8fb/0xf8e3  level24 [DEATHMATCH_GAME]; level0b
level24   mission KELLER CORE                    1      -     3     3   260    1570 0xf799/0xf891  dm1 [DEATHMATCH_GAME]
level3    mission NATAKA TOWER                   1    0x1     8     6    84       7 0xe064/0xe1af  level4 [DEATHMATCH_GAME]; level00
level4    mission OIL RIG                        3    0x7     5     5   166      11 0xde0d +2gen/0xde50  secrt1; level5 [DEATHMATCH_GAME]; level00
level5    mission OIL REFINERY                   3    0x7     4     4    91      40 0xe370/0xe48a  level6 [DEATHMATCH_GAME]; level00
level6    mission CADTLELTL                      2    0x3     6     6   181       6 0xdac6x2/0xdac4  level7 [DEATHMATCH_GAME]; level00; level7 [DEATHMATCH_GAME]; level0a
level7    mission GHOST TOWN                  1(2)    0x3     4     4   285       - 0xe3fb/0xe6c5  level8 [DEATHMATCH_GAME]; level00
level8    mission ARGUS BASE                     2    0x3     3     3   824       - 0xe3bd/0xe4d7  level9 [DEATHMATCH_GAME]; level00
level9    mission VIRUS LAB                      3    0x7     4     4   456       - 0xd75d/0xd91a  level10 [DEATHMATCH_GAME]; level000
dm1       deathmatch TRAILER PARK                   0      -     0     -     -       - 0xbe32/0xbe25  dm2
dm2       deathmatch FOREST                         0      -     0     -     -       - 0xbe6b/0xbe5e  dm3
dm3       deathmatch NIAC COMBAT ARENA              1      -     0     -     -       -  0xbdc5/-  dm4
dm4       deathmatch DEATHMATCH 'TILL DAWN          0      -     0     -     -       - 0xbe85/0xbe8e  level1
secrt1    secret SECRET LEVEL - FOUNDRY         0      -     4     4   147       - 0xd922/0xd93f  level5 [DEATHMATCH_GAME]; level00
secrt2    secret SECRET LEVEL - PROCESSOR       0      -     2     -   238       - 0xe021/0xe157  level19 [DEATHMATCH_GAME]; level000
temple    secret DM5 - TEMPLE                   0      -     1     -     -       - 0xbdfa/0xbe17  level5 [DEATHMATCH_GAME]; level00
```

`yourbest` — минимум из `GUNMETAL.HI` по имени уровня (в зеркале
`~/.cache/gunmetal-re/game` файла нет, поэтому там колонка пустая; числа выше —
прогон по реальной установке).

## 9. Воспроизведение

```
python3 tools/re/campaign_flow.py --self-test
python3 tools/re/campaign_flow.py --summary
python3 tools/re/campaign_flow.py --summary --game ~/.cache/gunmetal-re/game   # без GUNMETAL.HI
python3 tools/re/campaign_flow.py --json > /var/tmp/campaign_flow.json
python3 tools/re/campaign_flow.py --fields
```

Ожидаемо:

```
35/35 self-tests passed
level ... (36 строк)
ok: 24 missions, one counter step each, no gaps
GUNMETAL.HI: 9 records
```

`--self-test` работает без файлов игры: проверяет вложенность `IF` (обход
`campaign._walk`), нарезку под-скрипта по `END`, правило
`LEVEL_OBJECTIVES_COMPLETE`, семантику `NEXT_LEVEL`/роли, классификацию
выходов (DM/штаб/`NEXT_LEVEL`), склейку выходов нескольких `-8` (метка `xN`),
построение цепочки на синтетических 24
миссиях и обнаружение пропуска, формат `GUNMETAL.HI` (round-trip, минимум по
имени, обрезка/длинное имя), карту полей и детерминизм JSON.

`--fields` печатает 35 записей «поле → адрес → статус → писатель → читатель →
доказательство» (полный текст — в `FIELDS` инструмента).

## 10. Гипотезы и остатки

1. **Чем вызывается `entity 305` в миссиях, где нет явной ссылки.** В штабах
   это предмет с `E=305` (`ADD_ITEM_FIXED`, `0xc7e3`), в `level19` — тоже
   (`0xfad5`), в `level24` — `GENERATE 305` из `-30308` (`0xf849`). В
   `level1` и большинстве миссий ни `GENERATE`, ни `ADD_ITEM` с 305 в скрипте
   нет. Кандидат — именованный предмет `gate1`: в `NAMED_ITEMS` уровня
   `gate1` = `(0,10,0,0,68,305,0,100,0)`, `f5=305` идёт в `ITEM+0x44`
   (`docs/SBT_RECORDS.md` §2.5), а `+0x44` — это же поле, куда пишется `E`
   у `ADD_ITEM`. Что `+0x44` — «id триггера при подборе», **гипотеза**
   (потребитель поля не прослежен до конца). Проверять на живой игре или
   трассировкой `0x468xxx`.
2. **Бит `0x8000` в `player+0x3344`.** Именно он вместе с `0x83cb7c == 5`
   приводит к `event_post(-8)` (`0x445450`). В `docs/PLAYER_FIELDS.md` §«Биты
   +0x3344» он назван «синхронизация вида/башни»; в контексте конца уровня это,
   вероятно, «действие/продолжить» (второй канал управления, маска `0x8000`,
   `docs/CONFIG.md`). Семантика в этом месте — **гипотеза**.
3. **Автоматических титров после `level24` не найдено.** Проверено: 0 сайтов
   `END_GAME`, у `level24` нет кампанийного выхода, нет ссылок на
   `CREDITS`/`winner` в 36 скриптах. Титры запускаются пунктом меню
   (`0x4971e0`, индекс 3). Возможно, фронтенд включает пункт по факту
   прохождения — признак не найден (**гипотеза**).
4. **Сейв с целями/секретами.** Оба поставочных сейва — `level0`, поэтому
   восстановление `objectives_bits` через `-3` доказано кодом писателя и
   структурой блока 11, но не наблюдением на сейве миссии. Аналогично tally
   секретов в блоке 9.
5. **`level13`**: цель `DESTROY REACTOR` объявлена, но `OR_LEVEL_OBJECTIVES_BITS`
   в скрипте нет (0 бит), и `LEVEL_OBJECTIVES_COMPLETE` не проверяется. Похоже,
   цель там только текстовая; чем именно завершается миссия — отдельный
   триггер (не проверялось).
6. **`secrt2`/`temple`**: `INCREMENT_SOURCEPLAYER_SECRET_TALLY` есть, а
   `SET_LEVEL_SECRETS` нет → на экране «найдено / 0». Это данные поставки, не
   ошибка инструмента; в `docs/FORMATS.md` §15 у `secrt2` секреты тоже «—».
7. **`level15`**: 5 объявленных секретов, 4 сайта инкремента — один секрет,
   видимо, отмечается тем же сайтом/веткой; не проверялось на живой игре.

## 11. Поправки к существующим докам (файлы не правились)

| док | что сказано | что доказано здесь |
|---|---|---|
| `docs/FORMATS.md` §15 | «при загрузке сейва счётчик уменьшается на 1 (`0x44295e`)» | декремент — в `0x4428f0(param&1)`, это пункт «Restart level from HQ» (`0x4bd3fc`, `0x48da09`); загрузка сейва (`0x442340`) счётчик не меняет |
| `docs/PLAYER_FIELDS.md` | `+0x33cc` = `damage_accum` (unknown) | это `secret_tally`: `0x4536b0` (из `0x45b978`), сброс `0x45280f`, читают `0x49953a`/`0x483c07` |
| `docs/SAVE_FIELDS.md` F2 | «ставится в `0x442a0e`/`0x442cf7`» | плюс `0x442a4c` (вторая ветка новой игры); читается также брифингом `0x48f0f3`/`0x48f116` (counter и counter−1) |
| `docs/FORMATS.md` §15 | «`NEXT_LEVEL` встречается ровно там, где ожидается команда хаба» | ещё и `level19` (`0xf5a0`) — миссия сама двигает счётчик; 6 сайтов = 5 штабов + `level19` |
| `docs/VM_SCENE.md` §6 (`M5.2.4`) | «номер миссии в модели ВМ не реализован» | счётчик и выбор следующего уровня разобраны здесь; сама ВМ по-прежнему его не исполняет |
