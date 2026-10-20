# Исполнение SCRIPT-VM (`.sbt`, запись 11) — прототип

Статус: **прототип, чтение игры, без запуска `gundx.exe`**. Код:
`gm/sbtvm_exec.py` (интерпретатор), `tools/re/vm_exec.py` (CLI и корпусный
прогон), этот документ. Декодер остался в `gm/sbtvm.py` и не менялся.

До этого был только декодер/декомпилятор (`gm/sbtvm.py`) и статическое
извлечение сцены (`gm/scene.py`). Здесь добавлен **executor**: он ведёт
явное состояние (PC, стек подскриптов, флаги сущностей, переменные игроков,
глобалы уровня, ГПСЧ, сцена), исполняет байткод по тем же правилам, что
движок, и останавливается с диагностикой на всём, что не доказано.

## 1. Цикл ВМ в `gundx.exe`

Точка входа подскрипта — `0x457390(offset, …)`; буфер кода — `ds:0x4b6784`,
предел `0x30000` (`0x45739e`, `0x45c41b`). Восстановленный цикл:

| адрес | что делает |
|---|---|
| `0x4573cb` | `dl = code[pc]`; `0xff` → выход (`0x45c4f9`, это же обработчик `EXIT_EVENT`) |
| `0x4573db` | `0xc9` → `0x45d120` (ВМ условий), затем `0x45c412` |
| `0x457412` | отладочная ветка: при `[0x83d620]==1` команды не исполняются, а сверяются с таблицей (листинг/валидация) |
| `0x4574c0` | `op = code[pc++]`, `op-1 > 0xc9` → «unknown opcode» (`0x45c4d8`) |
| `0x4574d4` | `jmp [ (op-1)*4 + 0x45c500 ]` — диспетчер команд |

`IF` (`op 199`, обработчик `0x4574db`): считает условие через `0x45d120`
(в отладочном режиме — через `0x45f490`), затем

```
true  : pc += 4                  # 0x45be82 — пропустить dword размера
false : pc += *(u32*)&code[pc]   # 0x457569 — прыжок за then-ветку
```

`ELSE` (`op 200`, `0x45757a`) — **безусловный** `pc += *(u32*)&code[pc]`.
Важно: в скомпилированных уровнях `ELSE` стоит **последней инструкцией
then-ветки**, а не разделителем после неё (пример `dm1` `0x2900`–`0x2a4a`:
три `IF` подряд, каждый заканчивается `ELSE` на общую точку схода, а
«else-код» — это просто код после `IF`). Поэтому `IF false` попадает ровно
на инструкцию после then-ветки, т.е. на else-код; если then пуст
(`size == 4`), `IF` становится холостым тестом. Проверено на всех 36
уровнях: 11 674 `IF`, 268 `ELSE`, у всех `ELSE` цель — вперёд.

`0x45d120` (ВМ условий): читает `cid = code[pc++]`, `cid-1 <= 0x3a`,
`jmp [ (cid-1)*4 + 0x45f2ac ]`. Это отдельная таблица обработчиков на 59
записей — **не** стековая машина: каждый обработчик сам читает свои
инлайн-операнды, сравнивает их с глобальным состоянием и возвращает 0/1.
Арифметических опкодов в ВМ нет; «арифметика/сравнения» — это условия
`GAME_DIFFICULTY_AT_LEAST`, `SOURCEPLAYER_AMMO_AT_LEAST`, `RANDOM` и т.п.
(см. §4).

Собственный байтовый ридер (`gm.sbtvm_exec.decode_at`) сверен с декодером:
для **100 487** верхнеуровневых инструкций всех 36 уровней следующий PC
совпал с `gm.sbtvm.decode` (0 расхождений).

## 2. Модель состояния

`gm.sbtvm_exec.State`:

| поле | смысл | источник |
|---|---|---|
| `code`, `pc` | байткод и текущий PC | запись `SCRIPT` |
| `entities[id] = Entity(flags, offset)` | таблица сущностей | три колонки `flags/id/offset` (`gm.scene.entities`) |
| `stack`, `active` | стек подскриптов `GENERATE` (return PC, source) и множество активных offset | `0x479960` → `0x45fa00` → `0x457390` |
| `players[i]` | `playertype`, `key/weapon/equipment/behaviour/extra` биты, `cash` (+0x36c), `health`, `shield`, `ammo[]`, `equipment_active`, `weapon_index`, `salvage/morale/secrets` | поля структуры игрока `0x81c000 + i*0x110` |
| `source_player`, `source_entity`, `source_alt` | контекст «источника» (кто вызвал подскрипт) | `[ebp+0x28]`, `SOURCE_SWAP` |
| `game_mode`, `mp_flags`, `player_count`, `difficulty`, `level_number`, `god_mode` | контекст игры | `0x83d2c8`, `0x83d184`, `0x83d62c`, `0x80f9cc`, `0x62cad4` |
| `objectives`, `objectives_bits` | цели уровня | `DEFINE_LEVEL_OBJECTIVES` → `0x80d7ec/0x80f9d0` |
| `defs` | `DEFINE_AMMO/WEAPONS/EQUIPMENT/PLAYER_TYPES/SHIELDS/VIRUS/REGEN` | структурные опкоды |
| `scene` | `lights/locations/items/named_items/attached/sounds/sprites/things/doors/switches/lifts/spawns` | команды и `ADD_THING*` |
| `events` | всё исполненное, что не влияет на модель (звук, экран, камера, мир) | — |
| `clock`, `rng`, `occ`, `steps`, `trace` | виртуальное время `SLEEP`, ГПСЧ, счётчик повторных исполнений точки, бюджет, трасса | — |

Ограничения (все настраиваются): `max_steps` (по умолчанию 200 000),
`max_time` (5 с на прогон), `max_paths` (64), глубина стека `MAX_DEPTH = 32`.
Цикл `GENERATE` самого себя ловится по `active` (`recursion`), переполнение
стека — `stack-overflow`, выход PC за код — `bad-pc`.

Политика неопределённости: условие возвращает `True/False/None`
(`None` = модель не знает). `--on-unknown stop|false|true|both` — по
умолчанию `both`: состояние клонируется, исследуются обе ветки (сцена
объединяется по паре `(offset, occurrence)`). **Неизвестная команда не
пропускается молча**: путь останавливается с `unknown-opcode` /
`unsupported-command` и записью в трассе.

`GENERATE id` (`0x4575a3` → `0x479960`) переключает дверь с этим id и
исполняет подскрипт сущности (`0x45fa00` → `0x457390`), т.е. моделируется
настоящий вызов. `--script all` по умолчанию вызовы не разворачивает,
иначе один и тот же подскрипт считался бы дважды.

## 3. Команды: адреса, семантика, статус

Полная инвентаризация — 199 опкодов (192 именованных + 7 структурных; `0xc9`
`EVAL` обрабатывается циклом до диспетчера и в таблицу не входит). Адрес —
цель `jmp` из `0x45c500` (для `op 1..202`); статус:

* **structural** — размер/таблицы, не команда;
* **flow** — PC/вызовы/переходы;
* **scene** — создаёт объект в абсолютных координатах (реализовано);
* **attached** — создаёт объект, позиция берётся у рантайм-источника
  (фиксируется факт и аргументы, мировой позиции нет);
* **state** — меняет состояние модели (реализовано);
* **event** — звук/экран/камера/фильм/мутация мира: фиксируется, на модель
  не применяется (см. ограничения в §7);
* **unsupported** — таких в поставке не осталось; тест на неизвестный
  опкод синтетический (`0xfe`).

| op | command | signature | status | handler |
|---:|---|---|---|---|
| 1 | `EXIT_EVENT` | `` | flow | 0x45c4f9 |
| 2 | `GENERATE` | `E` | flow | 0x4575a3 |
| 3 | `GENERATE_FOR_CHILDREN` | `E` | event | 0x4575d3 |
| 4 | `GENERATE_FOR_PARENT` | `E` | event | 0x4576b1 |
| 5 | `ENABLE` | `E` | state | 0x457c67 |
| 6 | `DISABLE` | `E` | state | 0x457c30 |
| 7 | `SLEEP` | `P` | flow | 0x457729 |
| 8 | `SET_SECTOR_MOVEMENT` | `LLLLAAA` | event | 0x457815 |
| 9 | `SET_SECTOR_BRIGHTNESS` | `LL` | event | 0x4578d6 |
| 10 | `SET_SECTOR_TYPE` | `LL` | event | 0x457a1c |
| 11 | `SET_SECTOR_DAMAGE` | `LL` | event | 0x457961 |
| 12 | `SET_SECTOR_SOUND` | `LVND` | event | 0x4579ee |
| 13 | `SET_SECTOR_EVENTS` | `LEE` | event | 0x4579ab |
| 14 | `SET_SECTOR_FLAGS` | `LL` | event | 0x457929 |
| 15 | `SET_POLY_TEXTURE` | `LLLT` | event | 0x457a40 |
| 16 | `SET_POLY_BRIGHTNESS` | `LLLL` | event | 0x457a79 |
| 17 | `SET_POLY_SCROLL` | `LLLLLLL` | event | 0x457e7e |
| 18 | `LOG_MESSAGE` | `S` | event | 0x457ae3 |
| 19 | `EXIT_PROGRAM` | `` | flow | 0x457585 |
| 20 | `PLAY_SAMPLE` | `NVLLL` | event | 0x457d3e |
| 21 | `PLAY_SAMPLE_AT_LOCATION` | `NVLLLLL` | event | 0x457d7f |
| 22 | `SPEAK_NUMBER` | `L` | event | 0x457f13 |
| 23 | `SPEAK_SAMPLE` | `NVLL` | event | 0x457f2e |
| 24 | `SET_SOURCEWORLDOBJECT_HIT_OFF` | `` | event | 0x457f77 |
| 25 | `SET_SOURCEWORLDOBJECT_HIT_ON` | `` | event | 0x457f89 |
| 26 | `SET_SOURCEWORLDOBJECT_TEXTURES` | `T` | event | 0x457f9b |
| 27 | `SET_SOURCESECTOR_MOVEMENT` | `LLLAAA` | event | 0x457fc6 |
| 28 | `SET_SOURCESECTOR_BRIGHTNESS` | `L` | event | 0x458063 |
| 29 | `SET_SOURCESECTOR_FLAGS` | `L` | event | 0x458111 |
| 30 | `SET_SOURCESECTOR_TYPE` | `L` | event | 0x4580a9 |
| 31 | `SET_SOURCESECTOR_DAMAGE` | `L` | event | 0x4580cf |
| 32 | `SET_SOURCEPOLY_TEXTURE` | `T` | event | 0x458178 |
| 33 | `SET_SOURCEPOLY_BRIGHTNESS` | `L` | event | 0x4581ab |
| 34 | `SET_SOURCEPOLY_MATERIAL` | `M` | event | 0x458140 |
| 35 | `SET_SOURCEPOLY_SCROLL` | `LLLL` | event | 0x45821e |
| 36 | `SET_SOURCEPOLY_SWITCH_EVENT` | `E` | event | 0x4581fb |
| 37 | `PLAY_SAMPLE_AT_SOURCE` | `NVLL` | event | 0x457dbb |
| 38 | `PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE` | `LENVL` | event | 0x457df4 |
| 39 | `REMOVE_SOURCE_ITEM` | `` | event | 0x457bc0 |
| 40 | `SET_HANDLESECTOR_MOVEMENT` | `ELLLAAA` | event | 0x45829b |
| 41 | `SET_HANDLESECTOR_BRIGHTNESS` | `EL` | event | 0x4583a6 |
| 42 | `SET_HANDLESECTOR_TYPE` | `EL` | event | 0x458429 |
| 43 | `SET_HANDLESECTOR_DAMAGE` | `EL` | event | 0x458486 |
| 44 | `SET_HANDLESECTOR_FLAGS` | `EL` | event | 0x4584e7 |
| 45 | `SET_HANDLESECTOR_SOUND` | `ENVD` | event | 0x45833a |
| 46 | `SET_HANDLEPOLY_TEXTURE` | `LT` | event | 0x45853a |
| 47 | `SET_HANDLEPOLY_BRIGHTNESS` | `LL` | event | 0x4586d5 |
| 48 | `SET_HANDLEPOLY_SCROLL` | `LLLLL` | event | 0x4587a1 |
| 49 | `SET_HANDLEPOLY_MATERIAL` | `LM` | event | 0x458607 |
| 50 | `ADD_SCREEN_TEXT` | `LS` | event | 0x457b54 |
| 51 | `ADD_SOURCESCREEN_TEXT` | `LS` | event | 0x457b82 |
| 52 | `ADD_ITEM_AT_SOURCE` | `OPPEELP` | attached | 0x4599c5 |
| 53 | `ADD_ITEM` | `OPPEELLLLAAAP` | scene | 0x4598e7 |
| 54 | `ADD_ITEM_AT_SOURCE_FIXED` | `OPPEELP` | attached | 0x459a14 |
| 55 | `ADD_ITEM_FIXED` | `OPPEELLLLAAAP` | scene | 0x45994c |
| 56 | `SET_DOOR_DELTAS` | `LLLLAAA` | event | 0x45890a |
| 57 | `SET_DOOR_POSITION` | `LL` | event | 0x4589a5 |
| 58 | `SET_DOOR_SOUNDS` | `LNVNVNVD` | event | 0x458a1c |
| 59 | `SET_DOOR_SPEED` | `LL` | event | 0x458ab6 |
| 60 | `NEW_GAME` | `S` | flow | 0x457b39 |
| 61 | `NEW_LEVEL` | `S` | flow | 0x457af3 |
| 62 | `NEXT_LEVEL` | `` | flow | 0x457b11 |
| 63 | `END_GAME` | `` | flow | 0x45758f |
| 64 | `ITEMS_MOVE_CHECK` | `` | event | 0x457c26 |
| 65 | `SET_SOURCEITEM_TRIGGER_EVENT` | `E` | event | 0x457bf1 |
| 66 | `ADD_SPRITE` | `RDDDLLLLMDDL` | scene | 0x4592c2 |
| 67 | `ADD_SPRITE_AT_SOURCE` | `RLLLLLLLMDDL` | attached | 0x459382 |
| 68 | `ADD_SPRITE_AT_SOURCE_WEAPON_POINT` | `LRLMDDL` | attached | 0x45943e |
| 69 | `ADD_SPRITE_MOVING` | `RLLLLMDDLDDDP` | attached | 0x458b2d |
| 70 | `ADD_SPRITE_MOVING_AT_SOURCE` | `RLLLLLLLMDDLDDDP` | attached | 0x458b9e |
| 71 | `ADD_SPRITE_MOVING_AT_SOURCEPLAYER` | `RLLLLLLLMDDLDDDP` | attached | 0x458c76 |
| 72 | `ADD_SPRITE_MOVING_AT_SOURCE_WEAPON_POINT` | `LRLMDDLDDDP` | attached | 0x458fe5 |
| 73 | `ADD_SOURCEPLAYER_SPRITE` | `LRLLLLLLLMDDLL` | attached | 0x4596b4 |
| 74 | `REMOVE_SOURCEPLAYER_SPRITE` | `L` | event | 0x4598a5 |
| 75 | `CAUSE_BLAST_RADIUS_DAMAGE` | `LLLLLE` | event | 0x45775b |
| 76 | `SOURCE_CLEAR` | `` | event | 0x457749 |
| 77 | `SET_CAMERA_PITCH_SHAKE` | `APP` | event | 0x45b411 |
| 78 | `SET_CAMERA_ROLL_SHAKE` | `APP` | event | 0x45b462 |
| 79 | `SET_CAMERA_YAW_SHAKE` | `APP` | event | 0x45b4b3 |
| 80 | `SET_CAMERA_HAZE` | `DD` | event | 0x45b3bf |
| 81 | `LOAD_MUSIC` | `LS` | event | 0x45b5a0 |
| 82 | `PLAY_MUSIC` | `L` | event | 0x45b56a |
| 83 | `STOP_MUSIC` | `` | event | 0x45b612 |
| 84 | `UNLOAD_MUSIC` | `L` | event | 0x45b5f4 |
| 85 | `MEMORIZE_CURRENT_MUSIC_BANK` | `` | event | 0x45b629 |
| 86 | `PLAY_MEMORIZED_MUSIC_BANK` | `` | event | 0x45b63a |
| 87 | `ADD_LIGHT_SOURCE` | `LLLLLLLLLLLLLLLL` | scene | 0x45b662 |
| 88 | `ADD_SKY_LIGHT_SOURCE` | `LLLLLLL` | scene | 0x45b6c7 |
| 89 | `SET_SECTOR_LINK` | `LL` | event | 0x45889b |
| 90 | `SET_LEVEL_NAME` | `S` | state | 0x45b73c |
| 91 | `SHOW_LEVEL_END_SCREEN` | `` | event | 0x45c057 |
| 92 | `SET_LEVEL_END_CAMERA_XYZ` | `LLL` | state | 0x45b7ef |
| 93 | `SET_LEVEL_BEST_TIME` | `L` | state | 0x45b82a |
| 94 | `SET_LEVEL_SECRETS` | `L` | state | 0x45b816 |
| 95 | `INCREMENT_SOURCEPLAYER_SECRET_TALLY` | `` | state | 0x45b978 |
| 96 | `SET_LEVEL_GRAVITY` | `D` | state | 0x45b7db |
| 97 | `SET_LEVEL_FOG_COLOUR` | `LLL` | state | 0x45b7a0 |
| 98 | `SET_LEVEL_FOG_DISTANCE` | `L` | state | 0x45b7c7 |
| 99 | `SET_LEVEL_COUNTDOWN_TIMER` | `P` | state | 0x45b83d |
| 100 | `SET_LEVEL_FOCAL_ADJUST` | `D` | state | 0x45b78c |
| 101 | `SET_LEVEL_ALIASING_VALUES` | `DD` | state | 0x45b769 |
| 102 | `SET_LEVEL_VERSION` | `L` | state | 0x45b851 |
| 103 | `DEFINE_LEVEL_OBJECTIVES` | `` | structural | 0x45bdf0 |
| 104 | `OR_LEVEL_OBJECTIVES_BITS` | `L` | state | 0x45be40 |
| 105 | `AND_SOURCEPLAYER_KEY_BITS` | `L` | state | 0x45bb0c |
| 106 | `OR_SOURCEPLAYER_KEY_BITS` | `L` | state | 0x45bb50 |
| 107 | `AND_SOURCEPLAYER_WEAPON_BITS` | `L` | state | 0x45bb94 |
| 108 | `OR_SOURCEPLAYER_WEAPON_BITS` | `L` | state | 0x45bbd7 |
| 109 | `ADD_SOURCEPLAYER_AMMO` | `LL` | state | 0x459ca6 |
| 110 | `AND_SOURCEPLAYER_EQUIPMENT_BITS` | `L` | state | 0x45baaa |
| 111 | `OR_SOURCEPLAYER_EQUIPMENT_BITS` | `L` | state | 0x45badb |
| 112 | `HEAL_SOURCEPLAYER` | `LL` | state | 0x45af95 |
| 113 | `REPAIR_SOURCEPLAYER` | `LL` | state | 0x45b13d |
| 114 | `ADD_SOURCEPLAYER_CASH` | `L` | state | 0x459d99 |
| 115 | `ADD_SOURCEPLAYER_SALVAGE` | `L` | state | 0x459de9 |
| 116 | `CHANGE_SOURCEPLAYER_SOB` | `O` | event | 0x459efe |
| 117 | `SOURCEPLAYER_TURBO_BOOST` | `P` | event | 0x45a050 |
| 118 | `SOURCEPLAYER_RECOIL` | `D` | event | 0x459f4d |
| 119 | `SOURCEPLAYER_COLLISION_TYPE` | `LLLL` | event | 0x45a961 |
| 120 | `SOURCEPLAYER_HAZE` | `P` | event | 0x45a281 |
| 121 | `SOURCEPLAYER_HAZE_BRIGHTNESS` | `L` | event | 0x45a455 |
| 122 | `SOURCEPLAYER_INVINCIBLE` | `P` | event | 0x45ad04 |
| 123 | `SOURCEPLAYER_INFECT` | `` | event | 0x45a6aa |
| 124 | `SOURCEPLAYER_CURE` | `` | event | 0x45a730 |
| 125 | `SOURCEPLAYER_HIGHEST_WEAPON_WITH_AMMO` | `L` | event | 0x45b2bb |
| 126 | `SOURCEPLAYER_NEXT_WEAPON_WITH_AMMO` | `` | event | 0x45b2e1 |
| 127 | `SOURCEPLAYER_PREVIOUS_WEAPON_WITH_AMMO` | `` | event | 0x45b303 |
| 128 | `SOURCEPLAYER_FLY` | `L` | event | 0x45a8ee |
| 129 | `SOURCEPLAYER_HEADLIGHT` | `L` | event | 0x45ac91 |
| 130 | `SOURCEPLAYER_EQUIPMENT_ACTIVATE` | `LL` | state | 0x45a816 |
| 131 | `SOURCEPLAYER_FOOT_OFFSETS` | `DDDD` | event | 0x45a11d |
| 132 | `SOURCEPLAYER_DAMAGE_MODIFIER` | `DD` | event | 0x45a775 |
| 133 | `SOURCEPLAYER_DETECTOR` | `L` | event | 0x45a0aa |
| 134 | `SOURCEPLAYER_TARGETING` | `L` | event | 0x45af22 |
| 135 | `SOURCEPLAYER_TARGETED` | `L` | event | 0x45a195 |
| 136 | `SOURCEPLAYER_EQUIPMENT_INDEX` | `L` | state | 0x45a4be |
| 137 | `SOURCEPLAYER_WEAPON_INDEX` | `L` | state | 0x45a670 |
| 138 | `SOURCEPLAYER_WAKE` | `L` | event | 0x45abee |
| 139 | `SOURCEPLAYER_KILL` | `` | event | 0x45a557 |
| 140 | `SOURCEPLAYER_SELL_ALL` | `` | event | 0x45bc1a |
| 141 | `SOURCEPLAYER_DISABLE` | `E` | state | 0x457c9e |
| 142 | `SOURCEPLAYER_ENABLE` | `E` | state | 0x457cef |
| 143 | `SOURCEPLAYER_FOCAL_ADJUST` | `D` | event | 0x45a62d |
| 144 | `SOURCEPLAYER_STAND_GROUND` | `P` | event | 0x45a015 |
| 145 | `SOURCEPLAYER_FALL_PITCH_DOWN` | `DD` | event | 0x45a896 |
| 146 | `SOURCEPLAYER_RUB_SPEED_ADJUST` | `D` | event | 0x459fd2 |
| 147 | `SOURCEPLAYER_PLAY_LOOP` | `LNVLLD` | event | 0x45a5bc |
| 148 | `SOURCEPLAYER_STOP_LOOP` | `L` | event | 0x45a607 |
| 149 | `SOURCEPLAYER_STRAFE` | `DPD` | event | 0x45ad5e |
| 150 | `SOURCEPLAYER_Z_STRAFE` | `DPD` | event | 0x45ae09 |
| 151 | `SOURCEPLAYER_TARGET_LAG` | `P` | event | 0x45aeb4 |
| 152 | `SOURCEPLAYER_MOVEMENT_TYPE` | `Y` | state | 0x45a32e |
| 153 | `SOURCEPLAYER_SLEEP` | `` | event | 0x45a422 |
| 154 | `SOURCEPLAYER_DELETE` | `` | event | 0x45a3ce |
| 155 | `SOURCEPLAYER_MOVE_SPEED` | `D` | event | 0x45a387 |
| 156 | `SOURCEPLAYER_STEALTH` | `L` | event | 0x459e2d |
| 157 | `SOURCEPLAYER_TARGETED_MODIFIERS` | `DD` | event | 0x459ea6 |
| 158 | `SOURCEPLAYER_PATH` | `` | structural | 0x45b325 |
| 159 | `SOURCEPLAYER_PATH_INDEX` | `L` | event | 0x45b384 |
| 160 | `AND_SOURCEPLAYER_BEHAVIOUR_BITS` | `L` | state | 0x45b99a |
| 161 | `OR_SOURCEPLAYER_BEHAVIOUR_BITS` | `L` | state | 0x45b9de |
| 162 | `AND_SOURCEPLAYER_EXTRA_BITS` | `L` | state | 0x45ba22 |
| 163 | `OR_SOURCEPLAYER_EXTRA_BITS` | `L` | state | 0x45ba66 |
| 164 | `THING_ACCURACY_MODIFIERS` | `PDD` | event | 0x45b504 |
| 165 | `THING_TARGET_PLAYERS_MODIFIERS` | `DD` | event | 0x45b547 |
| 166 | `FREEZE` | `P` | event | 0x45b867 |
| 167 | `REFRESH_SCREEN` | `` | event | 0x45b893 |
| 168 | `SET_DEATHMATCH_START` | `LLLLAAA` | scene | 0x45c2bc |
| 169 | `SET_PLAYER_START` | `LLLLAAA` | scene | 0x45c270 |
| 170 | `DEFINE_WEAPONS` | `` | structural | 0x45c3fe |
| 171 | `DEFINE_AMMO` | `` | structural | 0x45c308 |
| 172 | `DEFINE_PLAYER_TYPES` | `` | structural | 0x45c33a |
| 173 | `DEFINE_VIRUS` | `LTNVLD` | state | 0x45c3a4 |
| 174 | `DEFINE_REGENERATION_MARKER` | `ODD` | state | 0x45c375 |
| 175 | `DEFINE_EQUIPMENT` | `` | structural | 0x45c321 |
| 176 | `DEFINE_SHIELDS` | `LL` | state | 0x45c353 |
| 177 | `ADD_RANDOM_AMMO_ITEM_AT_SOURCE` | `P` | attached | 0x45be5c |
| 178 | `ADJUST_SOURCEPLAYER_MORALE` | `L` | state | 0x45b89d |
| 179 | `FILM_DEFINE` | `LPLS` | event | 0x45c000 |
| 180 | `FILM_PLAY` | `` | event | 0x45c039 |
| 181 | `FILM_STOP` | `` | event | 0x45c043 |
| 182 | `FILM_REWIND` | `` | event | 0x45c04d |
| 183 | `ADD_SWITCH` | `LELLLLVN` | scene | 0x45b6ff |
| 184 | `ADD_DOOR` | `LEEEEEAAALLLLLLLDDDDDDDDDVNVNVN` | scene | 0x459b5b |
| 185 | `ADD_NAMED_ITEM` | `ILLLAAA` | scene | 0x459a78 |
| 186 | `ADD_NAMED_ITEM_EX` | `LLLILLLAAA` | scene | 0x459ac0 |
| 187 | `ADD_SOUND_SOURCE` | `LLLNVD` | scene | 0x459c6e |
| 188 | `ADD_LIFT` | `` | structural | 0x45be8d |
| 189 | `SHOW_AMMO_BUY_SCREEN` | `` | event | 0x45c0ae |
| 190 | `SHOW_ITEM_BUY_SCREEN` | `` | event | 0x45c109 |
| 191 | `SHOW_TANK_BUY_SCREEN` | `` | event | 0x45c157 |
| 192 | `SHOW_TANK_SAVE_SCREEN` | `` | event | 0x45c1a5 |
| 193 | `SHOW_WEAPON_BUY_SCREEN` | `` | event | 0x45c1f0 |
| 194 | `SHOW_BRIEFING_SCREEN` | `` | event | 0x45c23e |
| 195 | `SHOW_HELP_SCREEN` | `` | event | 0x45c257 |
| 196 | `ADD_LOCATION` | `ELLLAAA` | scene | 0x45bea6 |
| 197 | `DELETE_LOCATION` | `E` | scene | 0x45bf4f |
| 198 | `STOP_SOURCEPLAYER_SLEEPING_SCRIPTS` | `` | event | 0x45b93e |
| 202 | `SET_SOURCEPLAYER_TYPE` | `L` | state | 0x45bfbf |

### 3.1. Реализованная семантика (доказательство по обработчику)

| команда | операнды | что делает executor | доказательство |
|---|---|---|---|
| `IF` / `ELSE` / `EVAL` / `END` | см. §1 | ветвление/переход/выход | `0x4574db`, `0x45757a`, `0x4573db`, `0x4573d1` |
| `GENERATE` | `E` id | дверь: `toggles += 1`; сущность: вызов подскрипта | `0x4575a3` → `0x479960`, `0x44f690`, `0x45fa00` |
| `ENABLE` / `DISABLE` | `E` id | `flags |= 0x1ff` / `&= ~0x1ff` | `0x457c67` / `0x457c30` |
| `SOURCEPLAYER_ENABLE` / `_DISABLE` | `E` id | бит игрока `1<<(9+src)` (или `0x100`) в `flags` сущности | `0x457cef` / `0x457c9e` |
| `EXIT_EVENT` | — | возврат из подскрипта (`END` на глубине 0) | `0x45c4f9` |
| `EXIT_PROGRAM` | — | остановка (`program-exit`) | `0x457585` |
| `NEW_GAME/NEW_LEVEL/NEXT_LEVEL/END_GAME` | —/`S` | `level-transition` (чистая остановка) | `0x457b39/0x457af3/0x457b11/0x45758f` |
| `SLEEP` | `P` (4 байта) | `clock += operand`, пауза не блокирует | `0x457729` (сравнение с `[ebp+0xc]`) |
| `SOURCE_CLEAR` | — | `source = none` | `0x457749` (`&0xffff0000 | 0xffff`) |
| `SOURCEPLAYER_KILL` | — | `health = 0` | `0x45a557` |
| `OR_LEVEL_OBJECTIVES_BITS` | `L` | `objectives_bits |= a0` | `0x45be40` (`0x80f9d0`) |
| `AND/OR_SOURCEPLAYER_*_BITS` | `L` | `&= / |=` соответствующего поля игрока | `0x45bb0c/0x45bb50`, `0x45bb94/0x45bbd7`, `0x45baaa/0x45badb`, `0x45b99a/0x45b9de` |
| `ADD_SOURCEPLAYER_AMMO` | `L` тип, `L` дельта | `ammo[type] += delta`, кламп `[0, max]`; `max` — 2-е поле записи `DEFINE_AMMO` | `0x459ca6` (запись в `0x81c2dc`, кламп по `0x818964`) |
| `ADD_SOURCEPLAYER_CASH` | `L` | `cash += a0` | `0x459d99` (`[player+0x36c]`) |
| `HEAL/REPAIR_SOURCEPLAYER` | `L`, `L` | `health/shield += a1` (кламп по max) | `0x45af95/0x45b13d` |
| `SOURCEPLAYER_EQUIPMENT_ACTIVATE` | `L` бит, `L` вкл | `equipment_active` | `0x45a816` (`0x81c368`, `1<<bit`) |
| `SOURCEPLAYER_EQUIPMENT_INDEX` / `_WEAPON_INDEX` | `L` | текущий индекс | `0x45a4be` / `0x45a670` (`0x81c2d4`) |
| `SOURCEPLAYER_TYPE` / `SET_SOURCEPLAYER_TYPE` | `L` | `playertype = a0` | `0x45bfbf` |
| `SOURCEPLAYER_MOVEMENT_TYPE` | `Y` | `movement_type` | `0x45a32e` |
| `ADD_SOURCEPLAYER_SALVAGE/MORALE/...` | `L` | соответствующий счётчик | `0x459de9`, `0x45b89d`, `0x45b978` (по имени/семейству; **гипотеза о поле**) |
| `ADD_SKY_LIGHT_SOURCE` | 7/16 | свет в сцену | `0x47e130` (см. `docs/FORMATS.md` 12.4) |
| `ADD_LIGHT_SOURCE` | 16 | локальный свет | `0x47e1b0` |
| `ADD_LOCATION` | `ELLLAAA` | `Location(thing_id, pos=a1..a3)` | `0x45bea6` |
| `ADD_ITEM(_FIXED)` | `OPPEELLLLAAAP` | `Item(id=a1, pos=a5..a7)` | `0x4598e7` → `0x4686a0` |
| `ADD_SOUND_SOURCE` | `LLLNVD` | `SoundSource(pos=a0..a2)` | `0x459c6e` → `0x46b210` |
| `ADD_SPRITE` | `RDDDLLLLMDDL` | `Sprite(pos=a1..a3, jitter=a8)` | `0x4592c2` |
| `ADD_DOOR` | 31 операнд | `Door(flags, ids, angles, params, values, tail)` | `0x459b5b` → `0x44e250` |
| `ADD_SWITCH` | `LELLLLVN` | `Switch(kind,id,radius,pos=a3..a5)` | `0x45b6ff` → `0x44e5f0` |
| `ADD_LIFT` | `0x94 + n*0xc` | `Lift(head, stops)` | `0x45be8d` |
| `SET_PLAYER_START` / `SET_DEATHMATCH_START` | `LLLLAAA` | `Spawn(index=a0, pos=a1..a3, angles=a4..a6)` | `0x45c270` / `0x45c2bc` |
| `ADD_NAMED_ITEM` | `ILLLAAA` | `named_items` (позиция — **гипотеза**) | `0x459a78` |
| `ADD_NAMED_ITEM_EX` | `LLLILLLAAA` | `named_items` (id/позиция — **гипотеза**) | `0x459ac0` (обработчик сначала проверяет `a1/a2` по `0x83d62c`) |
| `DELETE_LOCATION` | `E` | удаляет `Location` с этим id | `0x45bf4f` |

`ADD_THING*` — не команды, а **условия** (см. §4): при `EVAL`/`IF` они
создают вещь через `0x46d000`, тип — операнд `G`, абсолютные координаты
только у `ADD_THING` (`GLLLAAA`) и `ADD_THING_EX` (`LLLGLLLAAAL`).

## 4. Условия (вторая ВМ, `0x45d120`)

Все 59 обработчиков — из таблицы `0x45f2ac`; колонка «prototype»:
`value` — значение считается из модели, `action` — создаёт объект/эффект,
`open` — честно неизвестно (политика `--on-unknown`).

| id | condition | signature | handler | prototype |
|---:|---|---|---|---|
| 1 | `ADD_THING` | `GLLLAAA` | 0x45d240 | action |
| 2 | `ADD_THING_EX` | `LLLGLLLAAAL` | 0x45d2c0 | action |
| 3 | `ADD_THING_AT_SOURCE` | `LGAAA` | 0x45d388 | action |
| 4 | `ADD_THING_AT_SOURCE_WEAPON_POINT` | `LLGDDDD` | 0x45d4bf | action |
| 5 | `ADD_THING_RELATIVE` | `LGLLLAAADDDD` | 0x45d881 | action |
| 6 | `GAME_DIFFICULTY_AT_LEAST` | `L` | 0x45e93f | value |
| 7 | `LEVEL_OBJECTIVES_COMPLETE` | `` | 0x45e9f0 | value |
| 8 | `NEXT_LEVEL_NUMBER` | `L` | 0x45ea26 | value |
| 9 | `SOURCE_IS_PLAYER` | `` | 0x45d15b | value |
| 10 | `SOURCE_IS_LOCAL_PLAYER_ONE` | `` | 0x45e278 | value |
| 11 | `SOURCE_IS_LOCAL_PLAYER_TWO` | `` | 0x45e29d | value |
| 12 | `DEATHMATCH_GAME` | `` | 0x45eaf9 | value |
| 13 | `DEATHMATCH_CLASSIC_GAME` | `` | 0x45eb06 | value |
| 14 | `DEATHMATCH_NORMAL_GAME` | `` | 0x45eb1c | value |
| 15 | `MULTIPLAYER_GAME` | `` | 0x45eb32 | value |
| 16 | `CAPTURE_THE_FLAG_GAME` | `` | 0x45eb81 | value |
| 17 | `TEAM_GAME` | `` | 0x45eb71 | value |
| 18 | `FIRE_SOURCEPLAYER_BULLET` | `LELBBLAAL` | 0x45e0a4 | action |
| 19 | `FIRE_SOURCEPLAYER_LASER` | `TMLELBBLAAD` | 0x45e119 | action |
| 20 | `FIRE_SOURCEPLAYER_THING` | `LLGLLLAAADAAAAD` | 0x45e19d | action |
| 21 | `NEAREST_OBSTRUCTION` | `LLLL` | 0x45ea4b | open |
| 22 | `NEAREST_OBSTRUCTION_FROM_SOURCE` | `L` | 0x45eaa4 | open |
| 23 | `PLAYERS_AT_LEAST` | `L` | 0x45eb4c | value |
| 24 | `RANDOM` | `D` | 0x45e965 | value |
| 25 | `RANDOM_FRAME_BALANCED` | `D` | 0x45e992 | value |
| 26 | `GOD_MODE` | `` | 0x45eb3f | value |
| 27 | `SOURCE_SWAP` | `` | 0x45d1b3 | value |
| 28 | `SCAVENGERHUNT_GAME` | `` | 0x45e26b | value |
| 29 | `SOURCE_FROM_GROUND` | `D` | 0x45ed14 | open |
| 30 | `SOURCEPLAYER_KEY_BITS_SET` | `L` | 0x45e2c2 | value |
| 31 | `SOURCEPLAYER_WEAPON_BITS_SET` | `L` | 0x45e321 | value |
| 32 | `SOURCEPLAYER_AMMO_FULL` | `L` | 0x45e464 | open |
| 33 | `SOURCEPLAYER_AMMO_AT_LEAST` | `LL` | 0x45e4cc | value |
| 34 | `SOURCEPLAYER_CASH_AT_LEAST` | `L` | 0x45e532 | value |
| 35 | `SOURCEPLAYER_EXTRA_BITS_SET` | `L` | 0x45eb91 | open |
| 36 | `SOURCEPLAYER_IS` | `L` | 0x45d185 | value |
| 37 | `SOURCEPLAYER_SHIELD_FULL` | `` | 0x45e6fe | value |
| 38 | `SOURCEPLAYER_LIFE_AT_LEAST` | `D` | 0x45e592 | value |
| 39 | `SOURCEPLAYER_LIFE_FULL` | `` | 0x45e67c | value |
| 40 | `SOURCEPLAYER_HAS_WEAPON_FOR_AMMO` | `L` | 0x45e81c | open |
| 41 | `SOURCEPLAYER_TELEPORT_HANDLE` | `E` | 0x45edfc | action |
| 42 | `SOURCEPLAYER_TELEPORT` | `DDDAAA` | 0x45ed80 | action |
| 43 | `SOURCEPLAYER_TYPE` | `L` | 0x45e380 | value |
| 44 | `SOURCEPLAYER_THING_TYPE` | `S` | 0x45e3d9 | open |
| 45 | `SOURCEPLAYER_WARP` | `P` | 0x45eece | action |
| 46 | `SOURCEPLAYER_WEAPON_POWER_UP` | `D` | 0x45e60f | action |
| 47 | `SOURCEPLAYER_UNDERWATER` | `` | 0x45e8dc | open |
| 48 | `SOURCE_UNDERWATER` | `` | 0x45e91c | open |
| 49 | `SOURCEPLAYER_SECTOR_FLAGS` | `L` | 0x45ebe8 | open |
| 50 | `SOURCEPOLY_HANDLE` | `E` | 0x45ec63 | open |
| 51 | `SOURCESECTOR_FLAGS` | `L` | 0x45ecd1 | open |
| 52 | `CUSTOM_COMMAND_0` | `` | 0x45efde | action |
| 53 | `CUSTOM_COMMAND_1` | `L` | 0x45f028 | action |
| 54 | `CUSTOM_COMMAND_2` | `LL` | 0x45f083 | action |
| 55 | `CUSTOM_COMMAND_3` | `LLL` | 0x45f0e2 | action |
| 56 | `CUSTOM_COMMAND_4` | `LLLL` | 0x45f146 | action |
| 57 | `CUSTOM_COMMAND_5` | `LLLLL` | 0x45f1af | action |
| 58 | `CUSTOM_COMMAND_6` | `LLLLLL` | 0x45f21d | action |
| 59 | `ADD_OTHER` | `ZLLLAAA` | 0x45d1f1 | action |

Ключевые доказательства:

| условие | обработчик | код |
|---|---|---|
| `RANDOM` | `0x45e965` | `rand() & 0xfff`, `×1/4095` (`0x4a2580`), `operand - x > 0` (`0x480e30`) |
| `GAME_DIFFICULTY_AT_LEAST` | `0x45e93f` | `operand <= [0x83d62c]` |
| `LEVEL_OBJECTIVES_COMPLETE` | `0x45e9f0` | все биты `0x80f9d0` по счётчику `0x80d7ec` |
| `NEXT_LEVEL_NUMBER` | `0x45ea26` | `[0x80f9cc] == operand` |
| `SOURCE_IS_PLAYER` | `0x45d15b` | `[source] & 0xffff < 8` |
| `SOURCEPLAYER_IS` | `0x45d185` | `[source] & 0xffff == operand` |
| `SOURCE_SWAP` | `0x45d1b3` | поворот двух 16-битных половин дескриптора источника | 
| `DEATHMATCH_CLASSIC/NORMAL` | `0x45eb06/0x45eb1c` | `[0x83d2c8] == 2/1` |
| `TEAM_GAME` / `CAPTURE_THE_FLAG_GAME` | `0x45eb71/0x45eb81` | `[0x83d184] & 1/2` |
| `GOD_MODE` | `0x45eb3f` | `[0x62cad4]` |
| `SOURCEPLAYER_AMMO_AT_LEAST` | `0x45e4cc` | `ammo[тип] >= операнд` (`0x81c2dc`, шаг игрока `0x110`) |
| `SOURCEPLAYER_CASH_AT_LEAST` | `0x45e532` | `[player+0x36c] >= операнд` |
| `SOURCEPLAYER_TYPE` | `0x45e380` | `[player+0x4a4] == операнд` |

## 5. CLI

```
python3 tools/re/vm_exec.py --level dm1
python3 tools/re/vm_exec.py --level dm1 --script all --trace
python3 tools/re/vm_exec.py --level dm1 --script id:-30077 --json
python3 tools/re/vm_exec.py --selftest
python3 tools/re/vm_exec.py --corpus --corpus-all
python3 tools/re/vm_exec.py --corpus --corpus-all --compare --contexts all --random-unknown
python3 tools/re/vm_exec.py --cmdtable
```

`--level` — путь или stem (`dm1`), `--game`/`GUNMETAL_GAME` — установка.
`--script`: `main` (offset 0), `all` (все offset из таблицы сущностей),
номер строки, `id:<signed id>`, `off:0x…`. `--max-steps`, `--max-time`,
`--max-paths`, `--seed`, `--on-unknown`, `--random-unknown`, `--follow`,
`--game-mode campaign|dm-normal|dm-classic|scavenger`, `--players`,
`--difficulty`, `--level-number`, `--god-mode`, `--playertype`,
`--source-player`. `--json` отдаёт состояние/счётчики/спавны (и
things/doors/switches при `--verbose`). `--corpus [DIR]` + `--compare` —
сверка с `gm.scene`, `--contexts all` — матрица `game_mode × playertype`.

`--selftest` не требует файлов игры; 17 тестов:

* `IF/ELSE` (then/else), вложенный `IF`;
* сравнение (`SOURCEPLAYER_CASH_AT_LEAST` после `ADD_SOURCEPLAYER_CASH`);
* неизвестное условие: `stop` и `both`;
* неизвестный опкод (`0xfe`);
* OOB PC (`ELSE` за конец блока);
* битый операнд (обрезанный dword, незавершённая строка);
* рекурсия `GENERATE` себя, переполнение стека (цепочка из 40 сущностей);
* `--max-steps` (step-limit);
* операнды `SET_PLAYER_START`, `ADD_THING_EX`, `DISABLE`.

## 6. Результаты по корпусу (36 `.sbt`)

Все прогоны — на установленной игре (только чтение), `--seed 0x12345678`,
один контекст (campaign, difficulty 1), если не указано иное.

| метрика | значение |
|---|---|
| уровней / сущностей (блоков) | 36 / 15 061 |
| главные скрипты (offset 0) | **36/36** доходят до `END` (38 путей: 2 форка) |
| все блоки, один контекст | 15 061 прогон, 19 988 путей; **15 028** блоков дошли до `END`, 33 — чистая остановка `NEW_LEVEL`/`NEXT_LEVEL`; все 15 061 терминальны |
| чистая остановка | 33 блока — `NEW_LEVEL`/`NEXT_LEVEL` (`level-transition`), не ошибка |
| исполнено | 78 421 инструкция, **156** различных команд; 19 535 условий, 42 различных |
| главные скрипты: инструкций | 263, 17 команд |
| неизвестных опкодов | 0 |
| `unsupported` команд | 0 (все 199 опкодов классифицированы) |

Полная матрица `--contexts all --random-unknown` (8 контекстов: campaign /
dm-normal × playertype 0..3): 120 488 прогонов, 191 796 путей + 268
`level-transition`.

### 6.1. Сверка с `gm.scene` (counts/ids)

Полная матрица, все 36 уровней:

| категория | `gm.scene` | executor | расхождение |
|---|---:|---:|---|
| lights | 4 470 | 4 470 | 0 |
| locations | 461 | 461 | 0 |
| items (безымянные) | 101 | 101 | 0 |
| sounds | 1 173 | 1 173 | 0 |
| sprites | 555 | 555 | 0 |
| doors | 1 779 | 1 779 | 0 |
| switches | 3 964 | 3 964 | 0 |
| lifts | 45 | 45 | 0 |
| spawns | 391 | 391 | 0 |
| things | 4 008 (top-level) | 7 617 исполнений | 0 потерянных top-level, 3 609 «лишних» вложенных |
| named_items | — (в `other`) | 4 305 | новая категория |
| attached (source-relative) | — (в `other`) | 6 373 | новая категория |

Один контекст (campaign, deterministic RANDOM): items 100/101, sounds
1172/1173, sprites 551/555, things 5 019. Разница — **не** ошибка
интерпретатора, а нетронутые ветки: `level13` теряет пару item+sound из
`IF DEATHMATCH_GAME`, `level7` — 3 спрайта (`RANDOM` + DM), `level8` — 1
спрайт (вложенный `RANDOM`). В матрице контекстов они исполняются.

### 6.2. Главное расхождение: `gm.scene.things()` считает только top-level

`gm/scene.py:things()` обходит инструкции блока, **не заходя в тела `IF`**,
поэтому видит 4 008 `ADD_THING*`. Рекурсивный статический обход даёт
**7 563** инструкции, а executor исполняет 7 617 (с учётом повторных
исполнений одной точки в циклах). То есть 3 555 вещей лежат внутри
условных веток и в `Scene.things` не попадают. Это не ошибка декодера —
это ограничение извлекателя; executor его снимает, но платит
зависимостью от контекста (какая ветка исполняется).

Пример (`dm1`, блок `0x2fd2`): `IF SOURCEPLAYER_TYPE 0 { IF RANDOM … {
EVAL ADD_THING_RELATIVE … } }` — статически 9 вещей, исполняется 3–9 в
зависимости от типа игрока и ГПСЧ.

### 6.3. Что осталось неисполненным

После матрицы контекстов остаются 38 статических `ADD_THING*`-точек:
`GAME_DIFFICULTY_AT_LEAST > 1` (level3/17/18), `LEVEL_OBJECTIVES_COMPLETE`
(level17) и `IF SOURCE_IS_PLAYER` (level13). Проверено: добавление
`difficulty=4`, «все цели выполнены» и `source_player=0` доводит все 7
затронутых уровней до **0** неисполненных точек. То есть это не
недостижимый код, а состояния игры, которых нет в базовом контексте.

## 7. Ограничения и недоказанное

* **Нет стека операндов и арифметических опкодов.** «Арифметика» — это
  отдельные обработчики условий; таблица §4 показывает, какие из них
  посчитаны, а какие честно `open`.
* **`event`-команды (117 из 199 опкодов) не моделируются.** Это звук,
  камера, UI, фильмы, мутации секторов/полигонов и часть `SOURCEPLAYER_*`
  (`INVINCIBLE`, `SELL_ALL`, `DELETE`, `FREEZE` и т.п.). Для сцены и PC они
  безопасны, но условие, читающее изменённое ими поле, может быть
  посчитано по устаревшему состоянию; такие условия либо `open`, либо
  опираются на реализованные `KILL`/`HEAL`. Такие условия либо в `open`, либо (для `LIFE_*`) зависят от
  `SOURCEPLAYER_KILL`/`HEAL`, которые реализованы.
* **Контекст источника огрублён.** `SOURCE_SWAP` (`0x45d1b3`) меняет
  половины одного дескриптора; в модели это обмен `source_entity` и
  `source_alt`, а `SOURCE_IS_PLAYER`/`SOURCEPLAYER_*` читают
  `source_player`. Для `--script all` источник по умолчанию не игрок;
  отсюда 38 точек §6.3.
* **`RANDOM`** — формула доказана (`rand()&0xfff / 4095`), но сам
  генератор `0x480e30` не восстановлен: используется LCG с `--seed`.
  `RANDOM_FRAME_BALANCED` смоделирован так же (гипотеза).
* **`ADD_NAMED_ITEM(_EX)`** — позиция/id взяты по порядку операндов
  (гипотеза); `ADD_NAMED_ITEM_EX` в обработчике сначала проверяет `a1/a2`
  по `0x83d62c`, что не объяснено. В сверке §6.1 эти объекты не участвуют.
* **`attached`** (спрайты/предметы «у источника») фиксируются без мировой
  позиции — её даёт только рантайм-источник; `gm.scene` их тоже не
  размещает.
* **Кадры координат** света/звука/дверей не пересматривались: см.
  `docs/FORMATS.md` 12.4 (двери — сырые операнды, свет/звук — маркеры).
* **Таблицы `DEFINE_*`** сохраняются, но интерпретируется только
  `DEFINE_AMMO` (макс. патронов) и `DEFINE_SHIELDS` (max shield);
  `DEFINE_PLAYER_TYPES` в модель здоровья не разворачивается (по умолчанию
  100/0).
* **`GENERATE_FOR_CHILDREN/PARENT`** отнесены к `event`: в их обработчиках
  (`0x4575d3`/`0x4576b1`) нет вызова `generate_event` `0x479960` /
  `generate_runner` `0x45fa00`, только постановка событий через
  `event_post` `0x4795b0`; подскрипты они не запускают.
* **Циклы** в ВМ возможны только через повторный `GENERATE`/`SLEEP`
  (обратных переходов нет: `ELSE` — `u32` вперёд). Рекурсия ловится по
  активным offset, а не по счётчику шагов.
* Пределы `max_steps/max_paths/max_time` могут обрезать покрытие; в
  отчёте это видно как `step-limit`/`path-limit`/`deadline` (в корпусе не
  встречались).

## 8. Воспроизведение

```
cd /home/nooverload/gunmetal-re
python3 tools/re/vm_exec.py --selftest
python3 tools/re/vm_exec.py --level dm1 --script all
python3 tools/re/vm_exec.py --corpus --corpus-all
python3 tools/re/vm_exec.py --corpus --corpus-all --compare --contexts all --random-unknown
```

Ожидаемо: 17/17 selftest; 36/36 главных скриптов до `END`; в
`--compare` — нули по всем категориям, кроме `things` (вложенные ветки) и
38 контекстно-зависимых точек из §6.3.
