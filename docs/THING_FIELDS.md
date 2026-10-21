# Thing type (`0x776d60`) и thing instance (`0x7ac080`): карта полей (M2.3.3)

Что здесь: точные базы и шаги двух таблиц, которые двигают весь «вещный»
рантайм Gunmetal, полная карта полей **обеих** структур с адресами
читателей/писателей, таблица «поле → C++ → тест», сверка с поставочными
`.sbt`/сейвами и воспроизводимые команды.

Смежное (не дублируется): `docs/SOE.md` — семантика `.soe` → 936-байтовая
запись типа (полная карта 77 полей, `tools/re/soe_semantics.py --json`);
`docs/THINGS_CPP.md` — C++-модель `Things`; `docs/AI_CPP.md`,
`docs/PROJECTILE_CPP.md` — потребители; `docs/RECORDS.md` §2 — что такое
`0x800b60`. Новое здесь — **runtime-запись `0x528`** (в `THINGS_CPP.md` §6,
п. 1 она значилась неразобранной) и карта копирования «тип → экземпляр».

Инструмент: `tools/re/thing_fields.py` (`--map`, `--verify`, `--extract`,
`--self-test`; только stdlib).

---

## 0. Коротко

| что | значение |
|---|---|
| типы, массив | **`0x776d60`**, шаг **`0x3a8`** (936) |
| счётчик типов | `u32` @ **`0x794160`** (`docs/SOE.md` §1) |
| типы в `.sbt` | запись 4: `u32 n ≤ 0x80` + `n × 936` (`+0x000 name[16]`, `+0x010 49×double`, `+0x198 132×u32`) |
| экземпляры, массив | **`0x7ac080`**, шаг **`0x528`** (1320), **260** слотов |
| конец массива | `0x7ac080 + 260·0x528 = 0x7ffd20`; аллокатор `0x46d5e0` стережёт `&DAT_007ac3e0 + i·0x528 < 0x800080` |
| аллокатор слота | `0x46d5e0` (первый слот с `flags_a & 1 == 0`, 0..0x103) |
| создание | `0x46d000` (абсолютная), `0x46e420` (относительная), копия полей типа — **`0x46d8b0`** |
| тик | `0x470c70` (switch по `+0x390 movement_type`), `0x4752d0` (тип 6) |
| сейв | блок 3: 260 × (`u8 present` + `0x528`); writer `0x46fb50`, reader `0x46fd90` |
| «вторичная таблица» `0x800bb0` | **не типы**: это `+0x50 flags` таблицы `SECTOR_STUFF` `0x800b60`, 256 × `0x58` (`docs/RECORDS.md` §2) |

---

## 1. Метод

1. `tools/ghidra/mcp.py` (headless Ghidra 12.1.4, `docs/GHIDRA.md` §4.1):
   `xrefs`/`decompile`/`disasm`/`batch`.
2. **Копия полей типа в экземпляр** взята из **дизассемблера** `0x46d8b0`
   (`MOV EAX,[ECX+0x776xxx]` → `MOV [EBX+0x7acxxx],EAX`, `EBX = slot·0x528`).
   Декомпилятор на этом x87-коде путает половины `double` и переиспользует
   регистры, поэтому пары «поле → поле» сверялись с asm (см. §5,
   `--extract` — автоматическая перепроверка по тексту декомпиляции).
3. Семантика полей — по **потребителям**: `0x470c70` (тик), `0x4703a0`
   (expire), `0x470420` (урон), `0x470500` (попадание), `0x470130`
   (удаление), `0x46d7b0` (коллизия), `0x46ca40` (визуал),
   `0x4752d0` (снаряд), `0x473a00`/`0x477f90` (выбор цели).
4. Числа — из поставочных файлов (`--verify`), а также из **сейва**
   `SAVEGAME/saved0.sbt`: блок 3 — это буквально 260 записей `0x528`,
   то есть реальные экземпляры движка. Это главная проверка карты: значения
   экземпляра совпадают с полями типа (см. §8).

Все адреса — VA `gundx.exe` 1.36 (`ImageBase 0x400000`).

---

## 2. `0x7ac3e0` vs `0x7ac3ec` vs `0x800bb0` — расхождение объяснено

**`0x7ac3e0` и `0x7ac3ec` — одна и та же запись.** Это не две базы, а два
поля нулевого слота, названные Ghidra по первому обращению:

| адрес | смещение в записи (база `0x7ac080`) | что лежит |
|---|---|---|
| `0x7ac080` | `+0x000` | `saved_position` (копия позиции объекта при сейве) |
| `0x7ac3e0` | **`+0x360`** | `flags_a` (копия `THING_TYPES+0x1a8`) — первое поле, которое пишет `0x46d8b0` |
| `0x7ac3ec` | **`+0x36c`** | `model_index` (индекс `SOB_TABLE`, `param_2` в `0x46d8b0`) |

Доказательства базы `0x7ac080`:

* writer сейва `0x46fb50`: `pbVar4 = &DAT_007ac3e0`, затем
  `fwrite(pbVar4 - 0x360, 0x528, 1, f)` — сохраняется ровно `0x528` байт
  начиная с `0x7ac080`;
* аллокатор `0x46d5e0`: `pbVar2 = &DAT_007ac3e0; … pbVar2 += 0x528` до
  `0x800080` → 260 слотов;
* `0x46c7d0` (сброс уровня) чистит `&DAT_007ac3bc (=+0x33c)` и
  `[+0x33c+0x24] (=+0x360)` с шагом `0x528` ровно 260 раз.

Отсюда же: инстанс-массив занимает `0x7ac080..0x7ffd20`, а `0x800080` —
просто верхняя граница проверки аллокатора (там же рядом глобалы
`0x7ffd20/0x7ffd30`).

**`0x800bb0` — не типы вещей.** Это `+0x50` (flags) записи `SECTOR_STUFF`:
массив `0x800b60`, **256 записей × 0x58** (`0x800b60..0x806360`),
инициализация `0x46b1b0`. Флаги (`&1`, `&2`, `&4`, `&8`, `&0x10`) читают
`0x444166`, `0x46bce5`, `0x46c652`, `0x444767`, `0x4727cb`; подробная карта
полей (`move`, `sound`, `damage`, `event_a/b`) — `docs/RECORDS.md` §2.
Индекс — **номер сектора** (0..255), а не id типа: в `0x472790` он берётся
из `inst+0x3c8` (сектор вещи), в `0x443690` — из поля игрока `0x81c27c`
(шаг структуры `0xd44`).

---

## 3. Структура A: `THING_TYPES` (`0x776d60`, 936 байт)

Полная карта 77 полей — `docs/SOE.md` §3 (40 proven / 22 hypothesis /
15 unknown). Здесь — то, что нужно для экземпляра, плюс одна **поправка**.

| offset | тип | поле | кому переходит (instance) | статус |
|---:|---|---|---|---|
| `+0x000` | `char[16]` | `name` | — (поиск `0x47cb00`) | proven |
| **`+0x010`** | `double` | **`speed`** | `+0x0a0` | **proven**, см. ниже |
| `+0x018` | `double` | `f18` | `+0x0a8` | unknown |
| `+0x020` | `double` | `turn_accel` | `+0x060` | hypothesis (`0x470c70`) |
| `+0x028` | `double` | `turn_max` | `+0x068` | hypothesis |
| `+0x030` | `double` | `radius` | `+0x078` | proven (`0x46d7b0`) |
| `+0x038/40/48` | 3×`double` | `radius_x/y/z` | `+0x080/88/90` | proven |
| `+0x050` | `double` | `f50` | `+0x098` | unknown |
| `+0x058` | `double` | `search_radius` | `+0x0b0` | proven |
| `+0x068` | `double` | `f68` | `+0x0b8` | unknown |
| `+0x070` | `double` | `arrive_radius` | `+0x0c0` | hypothesis |
| `+0x078` | `double` | `gravity` | `+0x0d8` | proven (`0x4752d0`, данные) |
| `+0x080` | `double` | `damping` | `+0x0c8` | proven (`0x4752d0`) |
| `+0x088` | `double` | `f88` | `+0x0d0` | unknown |
| `+0x090/98/a0` | 3×`double` | `col_f90/98/a0` | `+0x048/50/58` | hypothesis (скорость углов) |
| `+0x0a8..0c4` | 4×`double` | `f_a8..f_c0` | `+0x228..0x244` | unknown |
| `+0x0c8` | `double` | `falling_fear` | `+0x1d0` | proven |
| `+0x0d0` | `double` | `leash_distance` | `+0x290` | hypothesis |
| `+0x0d8..0x197` | 4×(3+3)×`double` | смещения/углы детей | не копируются | proven |
| `+0x198/19c` | 2×`u32` | `f198/f19c` | не копируются (гейт спавна) | hypothesis |
| `+0x1a0` | `u32` | `collision_kind` (0/1/2) | `+0x330` | proven |
| `+0x1a4` | `u32` | `sob_index` (модель) | `+0x36c` | proven |
| `+0x1a8/1ac/1b0` | 3×`u32` | `flags_a/b/c` | `+0x360/364/368` | hypothesis (биты — §6) |
| `+0x1b4` | `u32` | `movement_type` 0..18 | `+0x390` | proven |
| `+0x1b8` | `u32` | `event_low_health` | `+0x394` | hypothesis |
| `+0x1bc` | `u32` | `event_morale_failure` | `+0x514` | proven |
| `+0x1c0` | `u32` | `event_timeout` | `+0x37c` | proven |
| `+0x1c4` | `u32` | `event_death` | `+0x328` | hypothesis |
| `+0x1c8` | `i32` | `hits` | `+0x32c` | hypothesis |
| `+0x1cc` | `i32` | `target_zone_threshold` (200) | `+0x334` | proven |
| `+0x1d0` | `i32` | `target_history_offset` | `+0x338` | proven |
| `+0x1d4` | `i32` | `max_health` (0 = неуязвим) | `+0x398` (current) и `+0x39c` (max) | proven |
| `+0x1d8..1e4` | 4×`i32` | `shield0..3` (max) | `+0x3a0..3ac` (current) и `+0x3b0..3bc` (max) | proven |
| `+0x1e8` | `i32` | `expire_ticks` (91 Гц) | `+0x370` | proven |
| `+0x1ec` | `u32` | `event_expire` | `+0x374` | proven |
| `+0x1f0` | `u32` | `event_create` | `+0x3d4` | proven |
| `+0x1f4` | `u32` | `event_damaged` | `+0x518` | proven |
| `+0x1f8` | `u32` | `event_destroy` | `+0x3d8` | hypothesis |
| `+0x1fc/200` | 2×`u32` | `sound_id`, `sound_volume` | `+0x3d0/3cc` | proven / hypothesis |
| `+0x204` | `u32` | `leash_location` | — (заменяет `+0x278` при спавне) | proven |
| `+0x208/20c` | 2×`u32` | `texture_a/b` | `+0x3c0/3c4` | proven |
| `+0x210/214` | 2×`u32` | `timer_period/step` | `+0x3ec/3f0` | hypothesis |
| `+0x218` | `u32` | `weapon_count ≤ 8` | `+0x3f4` | proven |
| `+0x21c+4i` | 8×`u32` | `weapon_id[i]` | `+0x3f8+4i` | proven |
| `+0x23c/25c/27c+4i` | 8×`u32` | `weapon_p0/p1/p2` | `+0x418/438/458+4i` | hypothesis (band/range/cooldown) |
| `+0x29c/2bc/2dc+4i` | 8×`u32` | `weapon_p3/p4/p5` | `+0x0f8/138/178+8i` как `double` | unknown |
| `+0x2fc` | `u32` | `children_count ≤ 4` | — (`0x46d000` спавнит детей) | proven |
| `+0x300/310+4i` | 4×`u32` | `child_type/child_param` | — | proven |
| `+0x320` | `u32` | `path_count ≤ 32` | `+0x480` | proven |
| `+0x324+4i` | 32×`u32` | `path_point[i]` (id локаций) | `+0x484+4i` | proven |

Два расхождения имён между документами: `+0x1c4` в `docs/SOE.md` называется
`event_death`, а в `engine/src/world/things.h` — `collide_event` (оба верны:
`0x4752d0` постит его при попадании, `0x47573a` — когда кончились `+0x1c8`
hits); `+0x0d8` (`falling_fear`) не путать с `+0x0d8` экземпляра (gravity).

**Поправка к `docs/SOE.md`:** `+0x010` — это **`speed`**, а не «`off_z`».
`0x46d0fc`: `FLD [EBP+0x38]` (double-аргумент спавна) `FADD [EBX+0x7ac120]`
`FSTP [EBX+0x7ac120]`, т.е. «скорость экземпляра += добавка»; и данные
сейва: у `CRATE` `+0x010 = 0.0`, у экземпляра `+0x0a0 = 1.0`; у
`BAGCRTHQ` `0.1 → 1.1`. Именно `+0x010` читает `0x4752d0`
(`speed·dt` — шаг снаряда), и тест `test_things` ждёт `ROCKET = 18.0`,
`Grenade = 2.0`.

---

## 4. Структура B: runtime instance (`0x7ac080`, 1320 байт, 260 слотов)

`source` — откуда поле инициализируется в `0x46d8b0` (`-` = ноль/вычислено
при спавне), `evidence` — кто читает/пишет.

| offset | size | поле | source | evidence | статус |
|---:|---:|---|---|---|---|
| `+0x000` | 24 | `saved_position` (x,y,z) | `-` | `0x46fb50` кладёт позицию объекта перед дампом, `0x46fd90` — обратно | proven |
| `+0x018` | 24 | `angles_cur` (3 угла) | операнды `ADD_THING*` | `0x46d9xx`, `0x41e025` (матрица), `0x470c70`, `0x46ea40` | proven |
| `+0x030` | 24 | `angles_next` | `-` | `0x470c70`: копия `+0x18`, затем `+= rate·dt` | hypothesis |
| `+0x048` | 24 | `angle_rate` | `+0x090/98/a0` | `0x46d9b6..0x46d9f2`, `0x470c70` case 1 | proven |
| `+0x060` | 8 | `turn_accel` | `+0x020` | `0x46dd41`, `0x470c70` (`+0x58 -= dt·+0x60`) | proven |
| `+0x068` | 8 | `turn_max` | `+0x028` | `0x46dd59`, `0x470c70` | proven |
| `+0x070` | 8 | `angle_limit` | операнд спавна | `0x46df80`, `0x470c70` клампит `+0x40` | hypothesis |
| `+0x078` | 8 | `collision_radius` | `+0x030` | `0x46dc99`, `0x46d7b0` → `0x43c51f` | proven |
| `+0x080` | 24 | `collision_box` (rx,ry,rz) | `+0x038/40/48` | `0x46dcb1..0x46dce1`, `0x46d7b0` | proven |
| `+0x098` | 8 | `f50` | `+0x050` | `0x46dcf9` | unknown |
| `+0x0a0` | 8 | `speed` | `+0x010` (+добавка) | `0x46dd11`, `0x46d0fc`, `0x4752d0` | proven |
| `+0x0a8` | 8 | `f18` | `+0x018` | `0x46dd29` | unknown |
| `+0x0b0` | 8 | `search_radius` | `+0x058` | `0x46dc03`, `0x473a00`/`0x477f90` | proven |
| `+0x0b8` | 8 | `f68` | `+0x068` | `0x46daa6`, `0x470c70` | hypothesis |
| `+0x0c0` | 8 | `arrive_radius` | `+0x070` | `0x46dbe5` | hypothesis |
| `+0x0c8` | 8 | `damping` | `+0x080` | `0x46def9`, `0x4752d0` | proven |
| `+0x0d0` | 8 | `f88` (5.0 у снарядов) | `+0x088` | `0x46df05` | unknown |
| `+0x0d8` | 8 | `gravity` | `+0x078` | `0x46da76`, `0x4752d0` (`+0xf0 -= +0xd8·g·dt·-0.2`) | proven |
| `+0x0e0/0e8` | 8+8 | константа `1.0` | init | `0x46daf6`, `0x46db05` | proven |
| `+0x0f0` | 8 | `vertical_speed` | `-` | `0x4752d0` (vz), `0x470c70` | proven |
| `+0x0f8/138/178` | 8×8 | `weapon_p3/p4/p5[8]` | `+0x29c/2bc/2dc` | цикл `0x46dd..`, int → `double` | unknown |
| `+0x1b8` | 24 | `velocity` (x,y,z) | `-` | `0x470c70`: каждый тик обнуляется и набирается `m·dv·dt` | proven |
| `+0x1d0` | 8 | `falling_fear` | `+0x0c8` | `0x46da5e` | proven |
| `+0x1d8/1e0` | 8+8 | `fall_accel` / `fall_max` | `-` | `0x46daee/0x46daf8` (нули), `0x470c70` | hypothesis |
| `+0x1e8` | 8 | `fall_speed` | `-` | `0x470c70`: интегрируется, клампится в `+0x1e0` | proven |
| `+0x208/210` | 8+8 | `impulse_fwd/z` | `-` | `0x470c70` добавляют в `+0x1b8/+0x1c0/+0x1c8` | hypothesis |
| `+0x218` | 8 | константа `0.05` | init | `0x46dfa8` | proven |
| `+0x228` | 32 | `f_a8..f_c0` | `+0x0a8..0x0c4` | `0x46d9fe..0x46da52` | unknown |
| `+0x278` | 24 | `spawn_position` | операнды `LLL` | `0x46dbe9..`; заменяется локацией `+0x204` | hypothesis |
| `+0x290` | 8 | `leash_distance` | `+0x0d0` | `0x46dbbf` (0 при `radius ≤ 0`) | hypothesis |
| `+0x2f8` | 24 | `direction` (единичный) | строка матрицы `object+0x30` | `0x46e1xx`, `0x4752d0` | proven |
| `+0x310` | 4 | `parent` (слот или `-1`) | аргумент спавна | `0x46e073`, `0x470130`, `0x470c70` | proven |
| `+0x314` | 4 | `child_count` | `-` | `0x46e07b`, `0x46d000`, `0x470130` | proven |
| `+0x318` | 32 | `child_slots[8]` | `-` | `0x46d000` (`0x7ac398 + count·4`) | proven |
| `+0x328` | 4 | `event_death` | `+0x1c4` | `0x46da8e`, `0x4752d0` постит | proven |
| `+0x32c` | 4 | `hits` | `+0x1c8` | `0x46da9a`, `0x475615/0x47573a` | proven |
| `+0x330` | 4 | `collision_kind` | `+0x1a0` | `0x46dc8d`, `0x46e282` | proven |
| `+0x334` | 4 | `target_zone_threshold` | `+0x1cc` | `0x46db2c`, `0x473fb4` | proven |
| `+0x338` | 4 | `target_history_offset` | `+0x1d0` | `0x46db32`, `0x476523` | proven |
| `+0x33c` | 4 | `object` (handle) | `0x4277cd` | `0x46d000`, `0x46ca40`, почти все потребители | proven |
| `+0x340` | 16 | 4×`u32` = 0 | `-` | `0x46e0xx`, читателей нет | unknown |
| `+0x360` | 4 | `flags_a` | `+0x1a8` | `0x46d992`, `0x46d5e0`, `0x470c70` | proven |
| `+0x364` | 4 | `flags_b` | `+0x1ac` | `0x46d99e`, `0x470c70` | proven |
| `+0x368` | 4 | `flags_c` | `+0x1b0` | `0x46d9aa`, `0x470c70` | proven |
| `+0x36c` | 4 | `model_index` (`SOB_TABLE`) | `+0x1a4` | `0x46d8d0`, `0x4677b0`; **`DAT_007ac3ec`** | proven |
| `+0x370` | 4 | `expire_ticks` (счётчик) | `+0x1e8` | `0x46d97a`, `0x470c70` → `0x4703a0` | proven |
| `+0x374` | 4 | `event_expire` | `+0x1ec` | `0x46d986`, `0x4703a0` | proven |
| `+0x378` | 4 | `timeout_timer` | `-` | `0x46daeb`, `0x470c70` | proven |
| `+0x37c` | 4 | `event_timeout` | `+0x1c0` | `0x46dadc`, `0x470c70` | proven |
| `+0x380` | 4 | `render_byte` (→ `object+0x74`) | аргумент спавна | `0x46e0xx`, `0x46ca40` | hypothesis |
| `+0x384` | 4 | `effect_id` (по умолчанию `0x40`) | init | `0x46e219`, `0x46ca40` → `0x428421` | hypothesis |
| `+0x388` | 4 | `effect_timer` | `-` | `0x46e223`, `0x470c70`, `0x470500` | hypothesis |
| `+0x38c` | 4 | `anim_state` | `0x4284bb(object)` | `0x46e210`, `0x46ca40` | hypothesis |
| `+0x390` | 4 | `movement_type` | `+0x1b4` | `0x46deed`, `0x470c70` (switch) | proven |
| `+0x394` | 4 | `event_low_health` | `+0x1b8` | `0x46dabe`, `0x470c70`, `0x470420` | proven |
| `+0x398` | 4 | `health` | `+0x1d4` | `0x46dc21`, `0x470420`, `0x470500` | proven |
| `+0x39c` | 4 | `max_health` | `+0x1d4` | `0x46dc1b`, `0x470420` (событие на 50 %) | proven |
| `+0x3a0` | 16 | `shields[4]` (текущие) | `+0x1d8..1e4` | `0x46dc4b..`, `0x470420` | proven |
| `+0x3b0` | 16 | `shield_max[4]` | `+0x1d8..1e4` | `0x46dc2d..`, `0x470420` | proven |
| `+0x3c0` | 4 | `texture_a` | `+0x208` | `0x46df2f`, `0x46ca40` → `0x4289df` | proven |
| `+0x3c4` | 4 | `texture_b` | `+0x20c` | `0x46df35`, `0x46ca40` | proven |
| `+0x3c8` | 4 | `sector` (0..255, `-1` = нет) | `0x4360f1` (поиск сектора) | `0x470c70` (движение сектора), `0x472790` | proven |
| `+0x3cc` | 4 | `sound_volume` | `+0x200` | `0x46ded5` | hypothesis |
| `+0x3d0` | 4 | `sound_id` | `+0x1fc` | `0x46dee1` | proven |
| `+0x3d4` | 4 | `event_create` | `+0x1f0` | `0x46dc75`, `0x46e275` | proven |
| `+0x3d8` | 4 | `event_destroy` | `+0x1f8` | `0x46dc81`, `0x4702e9` | proven |
| `+0x3dc` | 4 | `type_index` | аргумент спавна | `0x46d96e` | proven |
| `+0x3e0` | 4 | `last_attacker` | `-1` | `0x46df62`, `0x470500` | proven |
| `+0x3e4` | 4 | `attack_cooldown` | `-` | `0x46df56`, `0x470c70`, `0x4752d0` | proven |
| `+0x3e8` | 4 | — | `-` | `0x46df68` | unknown |
| `+0x3ec/3f0` | 4+4 | `timer_period/step` | `+0x210/214` | `0x46dd71/0x46dd77` | hypothesis |
| `+0x3f4` | 4 | `weapon_count` | `+0x218` | `0x46dd87`, `0x4752d0` | proven |
| `+0x3f8` | 32 | `weapon_id[8]` | `+0x21c+4i` | `0x46de6e`, `0x4752d0` (event на попадании) | proven |
| `+0x418` | 32 | `weapon_p0[8]` | `+0x23c+4i` | `0x46de76`, `0x473def` | hypothesis |
| `+0x438` | 32 | `weapon_p1[8]` | `+0x25c+4i` | `0x46de7c`, `0x4752d0` (радиус) | hypothesis |
| `+0x458` | 32 | `weapon_p2[8]` | `+0x27c+4i` | `0x46de82`, `0x4752d0` (кулдаун) | hypothesis |
| `+0x478` | 4 | `anim_id` | аргумент спавна | `0x46e0xx` → `0x43b639`/`0x43b1b6` (.soo), `0x46fb50` | hypothesis |
| `+0x47c` | 4 | `owner` (0..7 игрок / вещь+8) | аргумент спавна | `0x46e09b`, `0x4752d0` (исключает владельца), `0x470500` | proven |
| `+0x480` | 4 | `path_count` | `+0x320` (`0x777080`) | `0x46e08d` | proven |
| `+0x484` | 128 | `path_point[32]` | `+0x324+4i` | цикл `0x46e0xx` | proven |
| `+0x504` | 4 | `attached` | `-1` | `0x46df7a` | unknown |
| `+0x508/50c` | 4+4 | `timer_a/b` | `-` | `0x46df8c/0x46df92`, `0x470c70` (`0xaaa` при истечении) | hypothesis |
| `+0x510` | 4 | `spawn_param` | аргумент | `0x46e032`, `0x46ea40` (`-1`) | unknown |
| `+0x514` | 4 | `event_morale` | `+0x1bc` | `0x46daca` | proven |
| `+0x518` | 4 | `event_damaged` | `+0x1f4` | `0x46db17` | proven |
| `+0x51c` | 4 | `move_timer` (знаковый) | `-` | `0x46dfbc`, `0x470c70` (фаза `fall_speed`) | hypothesis |
| `+0x520/524` | 4+4 | — | `-` | `0x46dfc2/0x46db23` | unknown |

**Где позиция?** В записи `0x528` живой позиции нет: она хранится в
модельном объекте (`*(object+0x00..0x18)`, создан `0x4277cd`), туда её пишет
спавн и двигают `0x470c70`/`0x46ea40`; запись держит только копию
`+0x278` (позиция спавна, её же перекрывает `leash_location`) и
`+0x000` (обновляется при сейве из объекта). Это объясняет, почему в
сохранённых экземплярах `+0x278 == 0`, а `+0x000` — реальные координаты.

**Ориентация:** `+0x018` — три абсолютных угла (порядок аргументов
`0x41e025`: `+0x20`, `+0x18`, `+0x28`), `+0x030` — сглаженные/целевые,
`+0x048` — производные. Проверка данными: в сейве там `0.0`, `0.0`,
`6.2159` (≈ 2π−0.067), `−2.0944` (−120°), `0.7854` (45°).

---

## 5. Копирование «тип → экземпляр» (`0x46d8b0`)

Проверено по asm (`MOV [EBX+0x7acXXX],reg` ← `MOV reg,[ECX+0x776XXX]`,
`EBX = slot·0x528`); `python3 tools/re/thing_fields.py --extract decompile.c`
перепроверяет карту из текста декомпиляции и падает при расхождении.

| экземпляр | тип | экземпляр | тип |
|---|---|---|---|
| `+0x048` | `+0x090` | `+0x360` | `+0x1a8` |
| `+0x050` | `+0x098` | `+0x364` | `+0x1ac` |
| `+0x058` | `+0x0a0` | `+0x368` | `+0x1b0` |
| `+0x060` | `+0x020` | `+0x370` | `+0x1e8` |
| `+0x068` | `+0x028` | `+0x374` | `+0x1ec` |
| `+0x078` | `+0x030` | `+0x37c` | `+0x1c0` |
| `+0x080` | `+0x038` | `+0x390` | `+0x1b4` |
| `+0x088` | `+0x040` | `+0x394` | `+0x1b8` |
| `+0x090` | `+0x048` | `+0x398` | `+0x1d4` |
| `+0x098` | `+0x050` | `+0x39c` | `+0x1d4` |
| `+0x0a0` | `+0x010` | `+0x3a0..3ac` | `+0x1d8..1e4` |
| `+0x0a8` | `+0x018` | `+0x3b0..3bc` | `+0x1d8..1e4` |
| `+0x0b0` | `+0x058` | `+0x3c0` | `+0x208` |
| `+0x0b8` | `+0x068` | `+0x3c4` | `+0x20c` |
| `+0x0c0` | `+0x070` | `+0x3cc` | `+0x200` |
| `+0x0c8` | `+0x080` | `+0x3d0` | `+0x1fc` |
| `+0x0d0` | `+0x088` | `+0x3d4` | `+0x1f0` |
| `+0x0d8` | `+0x078` | `+0x3d8` | `+0x1f8` |
| `+0x1d0` | `+0x0c8` | `+0x3ec` | `+0x210` |
| `+0x228` | `+0x0a8` | `+0x3f0` | `+0x214` |
| `+0x230` | `+0x0b0` | `+0x3f4` | `+0x218` |
| `+0x238` | `+0x0b8` | `+0x514` | `+0x1bc` |
| `+0x240` | `+0x0c0` | `+0x518` | `+0x1f4` |
| `+0x290` | `+0x0d0` | | |
| `+0x328` | `+0x1c4` | `+0x32c` | `+0x1c8` |
| `+0x330` | `+0x1a0` | `+0x334` | `+0x1cc` |
| `+0x338` | `+0x1d0` | | |

Колонки с шагом: `weapon_id[8]` `+0x3f8+4i ← +0x21c+4i`;
`weapon_p0/p1/p2` `+0x418/438/458+4i ← +0x23c/25c/27c+4i`;
`weapon_p3/p4/p5` `+0x0f8/138/178+8i ← +0x29c/2bc/2dc+4i` (int → `double`);
`path_point[32]` `+0x484+4i ← +0x324+4i`; `path_count +0x480 ← +0x320`.

Два «ложных» ребра, которые даёт наивная трассировка регистров (и которые
инструмент помечает как известные артефакты): `+0x38c` — это результат
`0x4284bb` (состояние `.soo`), а `+0x480` берётся из `0x777080`
(`type+0x320`), хотя в регистре ещё лежит `type+0x218`.

---

## 6. Биты флагов (`+0x360/364/368` ← `+0x1a8/1ac/1b0`)

| слово | бит | что делает | адрес |
|---|---|---|---|
| `flags_a` | `0x1` | слот занят: проверяют аллокатор, сейв, тик, удаление | `0x46d5e0`, `0x46fb7b`, `0x470c70`, `0x470130` |
| `flags_a` | `0x2` | ветка «сработать/взорваться»: `flags_b & 0x100`, дистанция `+0xb8`, событие `+0x394` | `0x470c70` |
| `flags_a` | `0x8` | щиты `+0x3a0..3ac` убывают на `dt·0x80d8b4`; когда все 0 — бит снимается; затем поиск касаний | `0x470c70` |
| `flags_a` | `0x10` | одна текстура (`0x80d880`) вместо анимации | `0x46ca40` |
| `flags_a` | `0x18` | выставляется при контакте с другой вещью | `0x470c70` |
| `flags_a` | `0x40` | временный эффект: таймер `+0x388`, состояние `+0x384` вместо `+0x38c` | `0x470c70`, `0x470500`, `0x46ca40` |
| `flags_a` | `0x100` | `0x428c4f(object)` при спавне | `0x46d000`, `0x46e420` |
| `flags_b` | `0x100` | разрешает ветку `flags_a & 2` | `0x470c70` |
| `flags_b` | `0x2000` | не применять поворот родителя детям (`0x41e2f9` пропускается) | `0x46d000` |
| `flags_b` | `0x20000` | «заморожен»: тик выходит сразу | `0x470c70` (первая проверка) |
| `flags_b` | `0x40000000` | при падении пишет угол `-π/8` в `+0x38` (кроме `movement_type 5`) | `0x470c70` |
| `flags_c` | `0x2000/0x4000` | события `-24`/`-25` при смене сектора (`0x42d99f`/`0x42efc3`) | `0x470c70` |
| `flags_c` | `0x40000` | `0x4285d9(object)` | `0x46d000` |

Все 2896 поставочных типов имеют `flags_a & 1` — это и есть контракт
«слот занят» (проверяется в `--self-test`).

---

## 7. Поле → C++ → покрытие тестом

C++ (`engine/src`) **не** моделирует запись `0x528`: `gm::Thing` — это
событие `ADD_THING*` плюс разрешённый тип. Поэтому таблица связывает поле
типа с тем, что реально читают `Things`/`Ai`/`Projectiles`.

| поле (type) | C++ | тест |
|---|---|---|
| `+0x010 speed` | `Thing::speed`, `ProjectileSpawn::speed` | `test_things` ROCKET 18.0 / Grenade 2.0 |
| `+0x030 radius` | `Thing::radius`, swept sphere | `test_things` 10.0 |
| `+0x038/40/48 box` | `Thing::box[3]` | `test_things` 4/10/4 |
| `+0x058 search_radius` | `Thing::search_radius` | `test_things` 6000.0 |
| `+0x070 arrive_radius` | `Thing::arrive_radius`, `AiConfig::move_arrive_radius` | `test_ai` arrived |
| `+0x078 gravity` | `ProjectileSpawn::gravity` | `test_things` 1.0 (Grenade) / 0.0 (ROCKET) |
| `+0x080 damping` | `ProjectileSpawn::damping` | `test_things` 0.8 |
| `+0x0d0 leash` | `Thing::leash_distance` | `test_ai leash_and_return` |
| `+0x1a0 shape` | `Thing::collision_shape` | `test_things` 1/2 |
| `+0x1a4 model` | `Thing::model`, `model_index` | `test_things`, корпус `things_full` |
| `+0x1a8/1ac/1b0 flags` | `Thing::flags` (только флаги сущности) | нет |
| `+0x1b4 movement` | `Thing::movement_type`, `ai_path_mode` | `test_things` 6; `test_ai` path |
| `+0x1b8 low health` | не подключено | нет |
| `+0x1bc morale` | не подключено | нет |
| `+0x1c0 timeout` | `Thing::timeout_event` | `test_events` 1004 |
| `+0x1c4 death` | `Thing::collide_event` | `test_things` −30103 / −30094 |
| `+0x1c8 hits` | `Thing::hits_left`, `ProjectileSpawn::hits_left` | `test_things` 0/1 |
| `+0x1cc target zone` | `Thing::target_zone_threshold` | `test_things` 200 |
| `+0x1d0 history` | `Thing::target_history_offset`, `ai_history_lag` | `test_ai target_history` |
| `+0x1d4 max health` | `Thing::max_health/health` | `test_things` 2000/6000/10000 |
| `+0x1d8..1e4 shields` | `Thing::shields/shield_max` | `test_things` 0/500 |
| `+0x1e8 expire` | `Thing::expire_ticks`, `Things::tick` | `test_things expire_lifetime` |
| `+0x1ec expire ev` | `Thing::expire_event` | `test_things` −30093 / −30103 |
| `+0x1f0 create` | `Thing::create_event`, `Things::spawn` | `test_things type_events` |
| `+0x1f4 damaged` | `Thing::damaged_event` | `test_events` 1001 |
| `+0x1f8 destroy` | `Thing::destroy_event`, `Things::remove` | `test_events` 1005 |
| `+0x1fc/200 sound` | не подключено | нет |
| `+0x204 leash loc` | не подключено (таблица локаций есть) | нет |
| `+0x208/20c tex` | не подключено (рендер) | нет |
| `+0x210/214 timer` | не подключено | нет |
| `+0x218 weapons` | `Thing::proximity`, `AiWeapon` | `test_things proximity`, `test_ai` |
| `+0x21c+4i id` | `ProximityEntry::event`, `AiWeapon::event` | `test_things` event −30094 |
| `+0x23c/25c band` | `ProximityEntry::low/high` | `test_things` high 20 |
| `+0x27c cooldown` | `ProximityEntry::cooldown` | `test_things proximity` |
| `+0x29c/2bc/2dc off` | `ProximityEntry::offset` | `test_things proximity` |
| `+0x2fc..310 children` | не подключено (спавн детей — движок) | нет |
| `+0x320/324 path` | `Thing::path_locations`, `Ai::set_path_from_thing` | `test_ai path_locations` |

Пробелы (приоритет следующего шага): `turn_accel/turn_max/angle_rate`
(движение 1), `fall_*`/`impulse_*` (движение 6), `hits` против `expire`,
`timer_period/step`, `weapon_p3/p4/p5`, `anim_id/anim_state`, `+0x38c`.

---

## 8. Реальные данные

`--verify` по 36 поставочным `.sbt` + `SAVEGAME/saved0.sbt` (файлы только
читаются):

| метрика | значение |
|---|---|
| уровней | 36 |
| записей `THING_TYPES` | **2896** |
| model ref (`+0x1a4`) валиден | **2896/2896** |
| `flags_a & 1` | **2896/2896** |
| `movement_type` | 6:1636, 0:399, 15:280, 5:251, 1:98, 14:95, 18:61, 8:36, 12:17, 10:12, 2:8, 9:3 |
| `max_health == 0` (неуязвим) | 150 типов, максимум 999999 |
| `expire_ticks > 0` | 2143 типа |
| `weapon_count > 0` | 1016 типов (максимум 4) |
| `path_count > 0` | 30 типов (максимум 28) |
| щиты > 0 | 1609 типов |
| `hits > 0` | 1610 типов |

Позиционные уровни (те же числа закреплены в `test_things.cpp`):
`level0` — 57 типов, `dm1` — 59, `level0` `movement {0:10, 1:1, 5:1, 6:38,
8:1, 10:1, 15:4, 18:1}`.

Три примера типов (`level0.sbt`):

```
[  0] bgchunk1  model=BIGCHUNK move=6  hp=2000  shields=[0,0,0,0]  hits=0 expire=145  flags=00000005/00000006/00000000 speed=0.000  radius=10.0 search=6000.0 weap=0 path=0
[ 28] Grenade   model=GRENADE  move=6  hp=2000  shields=[0,0,0,0]  hits=0 expire=364  flags=00000005/00000838/00506000 speed=2.000  radius=10.0 search=6000.0 weap=1 path=0
[ 31] ROCKET    model=ROCKET   move=6  hp=800   shields=[0,500,0,500] hits=1 expire=728 flags=00000085/0000080c/00306000 speed=18.000 radius=10.0 search=6000.0 weap=0 path=0
```

**Сверка экземпляров с типами** (`saved0.sbt`, блок 3: 6 живых слотов из
260, `0x2f6da..0x316ce`):

| слот | тип | модель | совпало с типом |
|---:|---|---|---|
| 0 | 53 `BAGCRTHQ` | 65 `BAGCART1` | `move=10` (pathloop), `path=9`, `hp=2500/2500`, `speed=1.1 = 0.1+1.0` |
| 1 | 54 `HQTICK` | 16 `TICK` | `move=0`, `hp=6000`, щиты 6000/6000 (`+0x1d8` = max) |
| 4 | 56 `CRATE` | 66 `CRATE6` | `move=18` (pushable), `radius=40`, `hp=20000`, `speed=1.0 = 0.0+1.0` |

То есть `+0x36c`, `+0x390`, `+0x398/39c`, `+0x3a0/3b0`, `+0x480`, `+0x0a0`
и `+0x018` подтверждены на живых данных движка, а не только кодом.

---

## 9. Доказано / гипотеза / осталось

**Доказано (адреса):**

* базы и шаги: типы `0x776d60` × `0x3a8`; экземпляры `0x7ac080` × `0x528`,
  260 слотов (`0x46d5e0`, `0x46c7d0`, `0x46fb50`);
* `0x7ac3e0 = +0x360 flags_a`, `0x7ac3ec = +0x36c model_index` — одна запись;
* карта копирования `0x46d8b0` (§5) на уровне инструкций;
* семантика: `+0x0a0 speed` (в т.ч. `+=` добавка в `0x46d0fc`), `+0x078`
  радиус, `+0x080..0x090` бокс (`0x46d7b0` → `0x43c51f`), `+0x370/+0x374`
  expire (`0x470c70` → `0x4703a0`), `+0x398/39c` HP и `+0x3a0/3b0` щиты
  (`0x470420`), `+0x390` движение (`0x470c70`), `+0x3c8` сектор
  (`0x4360f1` + `0x472790`), `+0x47c` владелец и `+0x328` событие смерти
  (`0x4752d0`), `+0x310/314/318` родитель/дети (`0x470130`);
* `0x800bb0` — `SECTOR_STUFF+0x50`, а не таблица типов;
* `+0x010` = `speed` (поправка к `docs/SOE.md`).

**Гипотезы:** биты флагов §6, `+0x030` (сглаженные углы), `+0x208/210`
импульсы, `+0x1d8/1e0` параметры падения, `+0x0d8` drag, `+0x38c/384`
состояние `.soo`, `+0x478 anim_id`, `+0x508/50c/51c` таймеры,
`weapon_p0/p1/p2` (band/range/cooldown), `+0x278 spawn_position`
(писатель известен, читателя в сборке не нашлось).

**Осталось:**

1. Движение: `0x474ea0`, `0x474a10`, `0x474260`, `0x475930`, `0x476070` —
   по одной функции на `movement_type`; карта полей готова, но сами
   автоматы (кроме типа 6 в `docs/PROJECTILE_CPP.md`) не разобраны.
2. `+0x208/210` — кто пишет (взрыв/сектор/скрипт?); в этой сборке читателей
   `+0x278` нет, но `+0x208/210` используются.
3. Точный смысл `weapon_p0/p1/p2` (сравнения в `0x473def/0x473dfc`) и
   `+0x438` как «радиус атаки» в `0x4752d0`.
4. C++: запись `0x528` не моделируется; из полей типа не подключены
   `event_low_health`, `event_morale`, `timer_*`, `anim_*`, `textures`.

---

## 10. Воспроизведение

```sh
cd /path/to/gunmetal-re

# карты полей (текст / JSON)
python3 tools/re/thing_fields.py --map
python3 tools/re/thing_fields.py --map --json | head -40

# проверка на поставочных данных
python3 tools/re/thing_fields.py --verify "/mnt/d/games/Gunmetal 3dfx/level0.sbt" \
        "/mnt/d/games/Gunmetal 3dfx/dm1.sbt" \
        "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt"

# самопроверка (синтетика + реальные файлы), exit != 0 при провале
python3 tools/re/thing_fields.py --self-test

# перепроверка карты копирования по декомпиляции спавна
python3 tools/ghidra/mcp.py decompile 0x46d8b0 > /tmp/spawn.c
python3 tools/re/thing_fields.py --extract /tmp/spawn.c

# точечные запросы в Ghidra
python3 tools/ghidra/mcp.py decompile 0x46d8b0 0x46d000 0x470c70 0x4752d0
python3 tools/ghidra/mcp.py xrefs 0x7ac3e0
python3 tools/ghidra/mcp.py disasm 0x46d968 --count 60
```

Сейв-блок 3 (`saved0.sbt`) — единственный источник **живых** записей `0x528`;
он же используется в `--self-test` как эталон инвариантов (`flags_a & 1`,
`health ≤ max_health`, `shield ≤ shield_max`, `movement_type ≤ 18`,
`type < n_types`, `model < n_sob`).
