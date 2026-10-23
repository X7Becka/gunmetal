# Отладчик и граф скриптов `.sbt` (`M11.4`, срез 1)

Статус: **готово для среза 1**: CLI-трасса, точки останова по смещению/опкоду/
событию, `--watch`, граф блока в DOT и Mermaid, таблица блоков, self-test без
файлов игры. Это CLI поверх готового интерпретатора — интерактивного UI нет.

| файл | что |
|---|---|
| `tools/editor/sbtdbg.py` | отладчик/граф + CLI + самопроверка |
| `docs/SCRIPT_DEBUG.md` | этот документ |

Зависимости: только stdlib и уже существующие модули — `gm.sbtvm` (таблицы
`COMMANDS`/`CONDITIONS`, `decode_at` через `gm.sbtvm_exec`),
`gm.sbtvm_exec` (сам интерпретатор), `tools/re/vm_exec.py` (выбор уровня/блока
и построение состояния), `tools/editor/sbtasm.py` (границы подскриптов).
Код интерпретатора **не копировался**: `sbtdbg.DbgExecutor` — подкласс
`gm.sbtvm_exec.Executor`, который перехватывает два его хука (`_trace` перед
инструкцией и `_event` после события).

---

## 1. Что умеет

| режим | что делает |
|---|---|
| `--run LEVEL` | исполняет подскрипт(ы) и печатает трассу: смещение, глубина вложенности (`GENERATE`), имя опкода, аргументы, **эффекты** шага |
| `--break-offset 0x…` | остановка **до** инструкции по абсолютному смещению в коде |
| `--break-op NAME` | остановка **до** инструкции с таким опкодом (`SLEEP`, `GENERATE`, `IF`, …) |
| `--break-event NAME` | остановка **после** события с таким именем (обработчик уже вернулся) |
| `--continue N` | после остановки исполнить ещё ≤ N инструкций (снова с точками останова) |
| `--watch FIELD` | печатать **только** изменения поля (`player.cash`, `vm.ticks`, `player.ammo[11]`, `scene.items`) |
| `--list-blocks LEVEL` | таблица подскриптов: смещение, размер, инструкций, `IF`, `ELSE`, `EVAL`, `DEFINE_*`, вызовы, `LOOP` |
| `--graph LEVEL --block 0x…` | граф потока управления блока в `--dot FILE` и/или `--mermaid FILE` |
| `--self-test` | синтетический `SCRIPT` (без игры): круг `encode_payload` → `parse_payload`, парсер точек останова, трасса, breakpoints, watch, граф |
| `--json` | те же данные одной структурой (для скриптов/CTest) |

Эффект шага — это разница состояния **до** и **после** инструкции:

* `event NAME (…)` — событие, которое эмитировал обработчик (`State.events`);
  у `SLEEP`/`GENERATE`/`SOURCE_CLEAR` в скобках примечание обработчика;
* `player.cash 0 -> 20000`, `player.ammo[11] 0 -> 3`,
  `player.salvage 0 -> 20000` — изменённые поля игрока;
* `vm.ticks 0 -> 18` (`SLEEP`), `vm.depth 0 -> 1` (`GENERATE` вызвал
  подскрипт), `vm.source_entity …` (смена источника);
* `scene.attached +1 (0 -> 1)` — что скрипт создал/удалил в сцене.

Ограничение: интерпретатор `gm.sbtvm_exec` **не моделирует позу игрока** —
у `Player` нет позиции, есть только `cash/health/shield/ammo/биты/…`. Поэтому
дамп состояния печатает `player.pos: not modelled` и, если в сцене есть
`SET_PLAYER_START`, первую точку спавна (`scene.spawns`). Позиции объектов,
которые скрипт создаёт, видны в `scene.*` и в `--json`.

---

## 2. CLI

```
python3 tools/editor/sbtdbg.py --run LEVEL [--script main|all|<i>|id:N|off:0x..]
        [--break-offset 0x..] [--break-op NAME] [--break-event NAME]
        [--continue N] [--watch FIELD] [--trace] [--trace-limit N] [--show N]
        [--source-player 0..7] [--difficulty N] [--game-mode …] [--no-follow]
        [--on-unknown stop|false|true|both] [--json]
python3 tools/editor/sbtdbg.py --list-blocks LEVEL [--json]
python3 tools/editor/sbtdbg.py --graph LEVEL [--block 0x..]
        [--dot FILE] [--mermaid FILE] [--json]
python3 tools/editor/sbtdbg.py --self-test [--json]
```

`--script` — как у `tools/re/vm_exec.py`: `main` (смещение 0), `all` (все
блоки из таблицы сущностей, без дублей), индекс строки таблицы, `id:<id>`
сущности, `off:<hex>` абсолютного смещения блока.

Смещения в трассе, `--break-offset` и `--script off:` — **абсолютные**
(смещение в `SCRIPT.code`), те же, что у `--block`. В тексте
`tools/editor/sbtasm.py` смещения **относительны началу блока**, поэтому
`ELSE -> 0x79` в тексте и `-> 0x700b` в трассе — одна и та же цель для блока
`0x6f92`.

Игра только читается; `--game` по умолчанию `/mnt/d/games/Gunmetal 3dfx`.
Переменные окружения не читаются (как у остальных `tools/editor/*`).

Коды возврата:

| код | когда |
|---|---|
| `0` | прогон/остановка как просили (`end`, `return`, переход уровня, `break-*`, `step-budget`) |
| `1` | интерпретатор сообщил проблему: `unknown-opcode`, `unsupported-command`, `unknown-condition` (при `--on-unknown stop`), `bad-operand`, `bad-pc`, `step-limit`, `deadline`, `recursion`, `stack-overflow`; или упал `--self-test` |
| `2` | ошибка входа: нет уровня/блока, неизвестный `--break-op`/`--break-event`/`--watch`, нет `--dot`/`--mermaid`, файл вывода не пишется |

`--on-unknown` по умолчанию **`false`**: отладчику нужен один
детерминированный путь. Так же, как `vm_exec`, можно попросить `stop`,
`true` или `both` (у `both` трасса относится к первому пути, в `--json`
есть `paths`). `--trace-limit` по умолчанию 50 инструкций на блок,
`--show` (сколько блоков при `--script all` печатать полностью) — 8.

`--json` для `--run` отдаёт `{level, game, blocks:[{script, off, reason,
paths, steps, notes, stops:[{reason, pc, detail, state}], trace:[{pc, depth,
op, args, effects}], watch:{field:[{pc, op, from, to}]}, state, scene}]}`;
для `--list-blocks` — `{level, blocks:[…], totals}`; для `--graph` —
`{level, block, size, nodes, edges, kinds, loops, files}`. `state` — это
`vm`/`player`/`scene_counts`/`spawns`/`event_counts` (все значения — простые
числа/строки/словари).

---

## 3. Трасса: init-подскрипты `level0` и `dm1`

```
$ python3 tools/editor/sbtdbg.py --run level0 --script main --trace-limit 10
=== level0.sbt main (off 0x0)
  reason=end steps=24 paths=1 scene=empty
  trace (10 of 24 executed instructions):
    00000000 IF                           DEATHMATCH_GAME
    000002fe DEFINE_AMMO                  n=15
    000005f1 IF                           DEATHMATCH_GAME
    00000e28 DEFINE_WEAPONS               n=19
    00001654 DEFINE_EQUIPMENT             n=13
    00001957 IF                           GOD_MODE
    00002166 IF                           DEATHMATCH_CLASSIC_GAME
    000027df IF                           SCAVENGERHUNT_GAME
    00002e58 DEFINE_PLAYER_TYPES          n=4
    000034c6 DEFINE_SHIELDS               50 0
```

```
$ python3 tools/editor/sbtdbg.py --run dm1 --script main --trace-limit 8
=== dm1.sbt main (off 0x0)
  reason=end steps=22 paths=1 scene=empty
  trace (8 of 22 executed instructions):
    00000000 DEFINE_AMMO                  n=15
    000002f3 DEFINE_WEAPONS               n=19
    00000b1f DEFINE_EQUIPMENT             n=12
    00000dea IF                           GOD_MODE
    000015f9 IF                           DEATHMATCH_CLASSIC_GAME
    00001c72 IF                           SCAVENGERHUNT_GAME
    000022eb DEFINE_PLAYER_TYPES          n=4
    00002959 DEFINE_SHIELDS               50 0
```

`steps=N` в строке `reason=…` — сколько инструкций **исполнено** (счётчик
отладчика). В дампе остановки `vm: steps=` — собственный счётчик
интерпретатора: он растёт на каждый *выбранный* PC, включая инструкцию, перед
которой сработала точка останова, поэтому может быть на 1–2 больше.

Фрагмент с эффектами и вызовом подскрипта (`level0`, блок `0x6f92`, ветка по
умолчанию: неизвестные условия считаются ложными):

```
$ python3 tools/editor/sbtdbg.py --run level0 --script off:0x6f92 --trace-limit 12
=== level0.sbt block (off 0x6f92)
  reason=end steps=31 paths=1 scene=attached=4
  notes:
    - unknown condition SOURCE_UNDERWATER at 0x6f92
    - IF SOURCE_UNDERWATER assumed false at 0x6f92
  trace (12 of 31 executed instructions):
    00006f92 IF                           SOURCE_UNDERWATER
    000070cb IF                           SOURCE_FROM_GROUND 96
    00007517 SET_CAMERA_HAZE              1 0.005  ; event SET_CAMERA_HAZE
    00007528 SLEEP                        18  ; vm.ticks 0 -> 18; event SLEEP (sleep 18)
    0000752d ADD_ITEM_AT_SOURCE_FIXED     52 0 0 0 0 0 109  ; scene.attached +1 (0 -> 1)
    0000754a ADD_ITEM_AT_SOURCE_FIXED     53 0 0 0 0 0 259  ; scene.attached +1 (1 -> 2)
    00007567 IF                           DEATHMATCH_GAME
    00007572 ADD_ITEM_AT_SOURCE_FIXED     41 4294967295(0xffffffff) 0 4294937147(0xffff8a3b) 0 1600 5460  ; scene.attached +1 (2 -> 3)
    0000758f ADD_ITEM_AT_SOURCE_FIXED     41 4294967295(0xffffffff) 0 4294937146(0xffff8a3a) 0 200 910  ; scene.attached +1 (3 -> 4)
    000075ac PLAY_SAMPLE                  66 64 128 22050 10000  ; event PLAY_SAMPLE
    000075c1 IF                           DEATHMATCH_GAME
    000075fe CAUSE_BLAST_RADIUS_DAMAGE    0 0 80000(0x13880) 0 2250 0  ; event CAUSE_BLAST_RADIUS_DAMAGE
```

`notes` перечисляют условия, значение которых модель не может доказать
(`SOURCE_UNDERWATER`, `SOURCE_FROM_GROUND`), и принятое решение. Это не
ошибка: политика `--on-unknown`.

---

## 4. Точки останова

`level0`, блок `0xbd8b` (подбирает коробку запчастей: `ADD_SOURCEPLAYER_CASH
20000`). Остановка ровно перед инструкцией и затем два шага:

```
$ python3 tools/editor/sbtdbg.py --run level0 --script off:0xbd8b \
      --source-player 0 --break-offset 0xbdb2 --continue 2
=== level0.sbt block (off 0xbd8b)
  reason=step-budget steps=5 paths=0 scene=empty

stop 1: break-offset (offset 0xbdb2)
  vm: pc=0xbdb2 steps=3 ticks=0 depth=0 events=1 source_entity=None source_player=0
  player[0]: cash=0 health=100 shield=0 ammo={} weapon_bits=0x0 key_bits=0x0
      playertype=0 movement_type=0 equipment=0x0 extra=0x0 behaviour=0x0 salvage=0 morale=0 secrets=0
      weapon_index=0 equipment_index=0 equipment_active=0x0
  player.pos: not modelled by gm.sbtvm_exec (no player pose); scene.spawns=0
  events (1): ADD_SOURCESCREEN_TEXT=1
  scene: empty
  at 0xbdb2  ADD_SOURCEPLAYER_CASH 20000

stop 2: step-budget (budget spent)
  vm: pc=0xbdbc steps=6 ticks=0 depth=0 events=1 source_entity=None source_player=0
  player[0]: cash=20000 health=100 shield=0 ammo={} weapon_bits=0x0 key_bits=0x0
      ...
  at 0xbdbc  REMOVE_SOURCE_ITEM
  trace (5 of 5 executed instructions):
    0000bd8b IF                           SOURCE_IS_PLAYER
    0000bd91 ADD_SOURCESCREEN_TEXT        1 "BOX OF SPARE PARTS - $20000"  ; event ADD_SOURCESCREEN_TEXT
    0000bdb2 ADD_SOURCEPLAYER_CASH        20000  ; player.cash 0 -> 20000
    0000bdb7 ADD_SOURCEPLAYER_SALVAGE     20000  ; player.salvage 0 -> 20000
    0000bdbc REMOVE_SOURCE_ITEM
```

Факты этой остановки:

* `stop.pc == 0xbdb2` — смещение ровно то, что просили; `cash` ещё `0`,
  т.е. инструкция **не** выполнена;
* второй стоп — исчерпан бюджет `--continue 2`: выполнены
  `ADD_SOURCEPLAYER_CASH` и `ADD_SOURCEPLAYER_SALVAGE` (`cash=20000`,
  `salvage=20000`), остановка перед следующей (`0xbdbc`);
* повторного выполнения инструкции нет: после остановки точка этого PC
  снимается на один шаг.

По опкоду — та же семантика «до»:

```
$ python3 tools/editor/sbtdbg.py --run dm1 --script off:0x2a8d --break-op SLEEP
stop 1: break-op (op SLEEP)
  vm: pc=0x2afc steps=4 ticks=0 depth=0 events=0 ...
  at 0x2afc  SLEEP 18
  trace (3 of 3 executed instructions):
    00002a8d IF                           SOURCEPLAYER_SECTOR_FLAGS 2
    00002a9c IF                           SOURCE_UNDERWATER
    00002aa7 ADD_SPRITE_MOVING_AT_SOURCEPLAYER …  ; scene.attached +1 (0 -> 1)
```

По событию — семантика «после»: событие уже в `State.events`, остановка на
следующей инструкции:

```
$ python3 tools/editor/sbtdbg.py --run level0 --script main --break-event GENERATE
stop 1: break-event (GENERATE fired at 0x35b7)
  vm: pc=0x35c7 steps=21 ticks=0 depth=1 events=3 source_entity=-30065 ...
  events (3): GENERATE=1, THING_ACCURACY_MODIFIERS=1, THING_TARGET_PLAYERS_MODIFIERS=1
  at 0x35c7  DISABLE 4294937230(0xffff8a8e)
```

`GENERATE` уже увело исполнение в подскрипт (`depth=1`, `source_entity`), но
`DISABLE` в нём ещё не выполнен.

---

## 5. `--watch`

Печатает только изменения поля и ту инструкцию, которая их сделала. Поле,
которое реально меняется: `player.cash` в блоке `0xbd8b` (`level0`):

```
$ python3 tools/editor/sbtdbg.py --run level0 --script off:0xbd8b \
      --source-player 0 --watch player.cash
=== level0.sbt block (off 0xbd8b)
  reason=end steps=7 paths=1 scene=empty
  watch player.cash: 1 change(s)
    0000bdb2  ADD_SOURCEPLAYER_CASH        0 -> 20000
```

По всему уровню (`all`, 350 блоков) та же инструкция находится сама:

```
$ python3 tools/editor/sbtdbg.py --run level0 --script all --source-player 0 \
      --watch player.cash | grep -A2 'off 0xbd8b'
--- level0.sbt entity 4000151 (off 0xbd8b)
  reason=end steps=7 paths=1 scene=empty
  watch player.cash: 1 change(s)
```

Второе реально меняющееся поле — `vm.ticks` (счётчик `SLEEP`), `dm1`,
блок `0x2a8d` (без исполнения `GENERATE`, чтобы не уходить в рекурсию):

```
$ python3 tools/editor/sbtdbg.py --run dm1 --script off:0x2a8d \
      --watch vm.ticks --no-follow
=== dm1.sbt block (off 0x2a8d)
  reason=end steps=6 paths=1 scene=attached=1
  watch vm.ticks: 1 change(s)
    00002afc  SLEEP                        0 -> 18
```

Поле без изменений печатается явно (это не ошибка):

```
$ python3 tools/editor/sbtdbg.py --run level0 --script off:0xbd8b \
      --source-player 0 --watch player.ammo
  watch player.ammo: no changes in 7 traced step(s)
```

Поля:

* `player.*` — все скалярные поля `gm.sbtvm_exec.Player`: `cash`, `health`,
  `shield`, `max_health`, `max_shield`, `key_bits`, `weapon_bits`,
  `equipment_bits`, `extra_bits`, `behaviour`, `equipment_active`,
  `weapon_index`, `equipment_index`, `movement_type`, `playertype`, `salvage`,
  `morale`, `secrets`; `player.ammo` (весь словарь) и `player.ammo[N]`
  (тип N), `player.ammo_max`;
* `vm.*` — `ticks` (= `clock`, сумма `SLEEP`), `steps`, `pc`, `depth`,
  `events` (счётчик), `source_entity`, `source_player`, `objective_bits`,
  `level_name`, `game_mode`;
* `scene.*` — счётчики `lights, locations, items, named_items, attached,
  sounds, sprites, things, doors, switches, lifts, spawns`.

`player.*` имеют значение, только если задан источник-игрок
(`--source-player N`) или скрипт сам его выбрал; иначе значение `None` и
изменений нет. `--watch` подавляет печать трассы (нужна и трасса — добавьте
`--trace`); `--trace-limit` должен быть > 0, потому что изменения считаются
по снимкам шагов.

---

## 6. `--list-blocks`

```
$ python3 tools/editor/sbtdbg.py --list-blocks level0 | head -4
block          size    ops    IF  ELSE  EVAL DEFINE  calls  LOOP
0x0           13767     39     9     9     1      9      1     0
0x35c7            6      2     0     0     0      0      0     0
0x35cd           34     10     0     0     4      0      0     0
...
$ python3 tools/editor/sbtdbg.py --list-blocks level0 | tail -2
total         62497   3028   397   267   190      9    168     7
350 block(s), 62497 byte(s)
```

`ops` — инструкций в блоке (линейный обход: тело `IF` и код после `ELSE`
считаются по одному разу), `IF/ELSE/EVAL` — структурные опкоды,
`DEFINE` — таблицы/структуры (`DEFINE_*`, `ADD_LIFT`, `SOURCEPLAYER_PATH`,
`DEFINE_LEVEL_OBJECTIVES`), `calls` — инструкции `GENERATE*`, `LOOP` — см. §7.

| уровень | блоков | байт | ops | IF | ELSE | EVAL | DEFINE | calls | LOOP |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `level0` | 350 | 62 497 | 3 028 | 397 | 267 | 190 | 9 | 168 | 7 |
| `dm1` | 363 | 83 990 | 3 474 | 407 | 267 | 223 | 7 | 200 | 6 |

---

## 7. Граф потока управления

`--graph LEVEL --block 0x…` строит граф одного подскрипта:

* узел на каждую инструкцию (порядок — по смещению, он же порядок в файле);
* `IF` — ромб, два ребра: `true` → первая инструкция then-ветки,
  `false` → байт сразу за then-веткой (там стоит `ELSE` или else-код);
* `ELSE` — шестиугольник, одно ребро `jump` → цель (всегда вперёд: поле
  `u32`, поэтому «назад» невозможно);
* `GENERATE`/`GENERATE_FOR_CHILDREN`/`GENERATE_FOR_PARENT` — ребро `call`
  в отдельный узел «call 0x… (entity N)»; смещение берётся из таблицы
  сущностей (`id → offset`); если цель лежит внутри этого же блока (скрипт
  вызывает себя), ребро помечается `loop` (в DOT — `color=red`);
* `END`/`EXIT_EVENT`/`EXIT_PROGRAM`/`NEW_LEVEL`/`NEXT_LEVEL`/`NEW_GAME`/
  `END_GAME` — терминальные узлы;
* ребро за пределы блока (в поставке не встречается) даёт узел `exit`.

`LOOP` — ребро, замыкающее цикл (в графе это ребро `loop`, а не отдельный
тип узла). В этом байткоде нет переходов назад, но
циклы делаются через `GENERATE`: строка сущности указывает внутрь того же
блока, и после `SLEEP` скрипт вызывает себя заново. Именно так устроен,
например, `dm1 0x2a8d` (`GENERATE -30069` → `0x2a8d`):

```
$ python3 tools/editor/sbtdbg.py --graph dm1 --block 0x2a8d \
      --dot /tmp/opencode/sbtdbg/loop.dot --mermaid /tmp/opencode/sbtdbg/loop.md
dm1.sbt block 0x2a8d: 122 bytes, 9 nodes (if=2, else=2, cmd=3, call=1, end=1), 10 edges, 1 loop(s)
wrote /tmp/opencode/sbtdbg/loop.dot
wrote /tmp/opencode/sbtdbg/loop.md
```

```
$ python3 tools/editor/sbtdbg.py --graph level0 --block 0x6f92 \
      --dot /tmp/opencode/sbtdbg/6f92.dot --mermaid /tmp/opencode/sbtdbg/6f92.md
level0.sbt block 0x6f92: 1871 bytes, 84 nodes (if=8, else=8, eval=8, cmd=58, call=1, end=1), 92 edges, 0 loop(s)
wrote /tmp/opencode/sbtdbg/6f92.dot
wrote /tmp/opencode/sbtdbg/6f92.md
```

Формат DOT (`--dot`):

```
digraph script {
  graph [rankdir=TB, labelloc=t, label="level0.sbt block 0x6f92 (0x6f92..0x76e1)"];
  node [shape=box, fontname="monospace", fontsize=10];
  edge [fontname="monospace", fontsize=9];
  n006f92 [label="0x006f92 IF SOURCE_UNDERWATER", shape=diamond];
  n006fc6 [label="0x006fc6 ADD_ITEM_AT_SOURCE_FIXED 53 0 0 0 0 0 259", shape=box];
  n006fe3 [label="0x006fe3 IF DEATHMATCH_GAME", shape=diamond];
  n006fe9 [label="0x006fe9 ELSE -> 0x700b", shape=hexagon];
  n006f92 -> n006f98 [label="true"];
  n006f92 -> n0070cb [label="false"];
  n006fc6 -> n006fe3 [label="next"];
  n006fe3 -> n006fee [label="false"];
  n006fe3 -> n006fe9 [label="true"];
  n006fe9 -> n00700b [label="jump"];
  n006fee -> n00700b [label="next"];
  n00700b -> n007028 [label="next"];
  ...
}
```

Узел вызова (из блока `dm1 0x2a8d`, где `GENERATE` замыкает цикл на себя):

```
  n002b01 [label="0x002b01 GENERATE 4294937227(0xffff8a8b)", shape=box];
  c002a8d [label="call 0x002a8d (entity -30069)", shape=box, style=dashed];
  n002b01 -> c002a8d [label="loop", style=dashed, color=red];
  n002b01 -> n002b06 [label="next"];
```

Формат Mermaid (`--mermaid`):

```
flowchart TD
    %% level0.sbt block 0x6f92 (0x6f92..0x76e1)
    n006f92{"0x006f92 IF SOURCE_UNDERWATER"}
    n006fc6["0x006fc6 ADD_ITEM_AT_SOURCE_FIXED 53 0 0 0 0 0 259"]
    n006fe3{"0x006fe3 IF DEATHMATCH_GAME"}
    n006fe9{{"0x006fe9 ELSE -#gt; 0x700b"}}
    n006fee["0x006fee ADD_ITEM_AT_SOURCE_FIXED 41 4294967295(0xffffffff) 0 4294937147(0xffff8a3b) 0 ..."]
    n006f92 -->|true| n006f98
    n006f92 -->|false| n0070cb
    n006fe3 -->|false| n006fee
    n006fe3 -->|true| n006fe9
    n006fe9 -->|jump| n00700b
```

Формы узлов: `["…"]` команда, `{"…"}` `IF`, `{{"…"}}` `ELSE`,
`[["…"]]` вызов подскрипта, `(["…"])` `EVAL`/`END`/`exit`. Символы, ломающие
разметку, экранируются (`"` → `#quot;`, `>` → `#gt;`, `|` → `#124;`).
Подписи содержат **абсолютные** смещения; `->` в подписи `ELSE` — цель
перехода, а не ребро графа.

Порядок узлов детерминирован (сначала инструкции по смещению, затем узлы
вызовов по цели), рёбра отсортированы по (узел-источник, метка, цель) —
повторный запуск даёт побайтово тот же файл. Проверено на `0x6f92`:
84 узла / 92 ребра в обоих форматах, все концы рёбер объявлены.

По всему корпусу (36 уровней, 15 061 блок): **157 230 узлов, 161 177 рёбер,
254 ребра-цикла в 242 блоках** (само-`GENERATE`; цель всегда — начало своего
блока), узлов `exit` нет.

---

## 8. Self-test

```
$ python3 tools/editor/sbtdbg.py --self-test
ok    synthetic SCRIPT: encode_payload(11) -> parse_payload round-trip
ok    synthetic SCRIPT: blocks 0x0 and 0x24
ok    break specs: offset/op/event parsed
ok    break specs: bad offset/op/event rejected
ok    trace: IF -> else branch (difficulty 1)
ok    trace: effects show player.cash 0 -> 250
ok    trace: GENERATE enters the sub-script (depth 1)
ok    trace: event LOG_MESSAGE in the effects
ok    break-offset: stops exactly there, before it runs
ok    break-offset + --continue 1: instruction runs once
ok    break-op: stops before GENERATE
ok    break-event: stops after the event, before the next instruction
ok    step budget: --continue 2 executes two instructions
ok    watch: only changes of player.cash
ok    watch: unknown fields rejected
ok    graph: IF, ELSE, call and END nodes present
ok    graph: call node resolves the entity offset
ok    graph: every edge endpoint is a node
ok    graph: forward-only CFG has no loops
ok    graph: a back edge is counted as a LOOP
ok    graph: DOT text is a digraph with all nodes/edges
ok    graph: Mermaid text is flowchart TD with all nodes/edges
ok    graph: node order is stable across two builds
ok    graph: self-recursive GENERATE gives a LOOP edge
ok    list-blocks: two blocks with their census
ok    state dump: vm/player/events/scene present

26/26 self-tests passed
```

Синтетический скрипт собирается без игры: `gm.sbt.Script` (две колонки
сущностей, код с `IF/ELSE`, `SLEEP`, `GENERATE` и подскриптом) проходит через
`gm.sbt.encode_payload(11, …)` и `parse_payload(11, …)`, после чего
отлаживается как обычная запись. (Функция `encode_payload` живёт в `gm.sbt`;
в `gm.sbtvm` её нет — там только таблицы опкодов и декодер.) Код инструкций собран синтетическим
кодировщиком интерпретатора (`gm.sbtvm_exec.enc_if`/`_enc_cmd`), поэтому
тест не зависит от файлов игры.

---

## 9. Ограничения

* **Нет интерактивного UI.** Только CLI: прогон, остановки по заранее
  заданным условиям, печать. Пошагового ввода команд, точек останова по
  условию (`player.cash > 100`) и обратного выполнения нет.
* **`--trace-limit` ограничивает и трассу, и `--watch`**: изменения
  считаются по снимкам шагов, а снимки хранятся только для первых N
  инструкций (плюс один шаг, чтобы показать эффект последней). Для длинных
  скриптов поднимите лимит или сузьте блок через `--script off:`/`id:`.
* **Один путь.** По умолчанию неизвестные условия считаются ложными
  (`--on-unknown false`), поэтому трасса детерминирована, но не показывает
  «а что было бы при истине». `--on-unknown both` форкает состояние, и тогда
  печатается трасса первого пути (`paths` в `--json`).
* **`--continue` действует только после остановки**: без точки останова
  прогон идёт до конца (`--continue` без `--break-*` ничего не ограничивает).
* **Позиция игрока не моделируется** (`gm.sbtvm_exec`): видны деньги, жизнь,
  щит, патроны, биты, инвентарь-индексы, тик `SLEEP`, события; поза — нет.
* **Часть эффектов — «события», а не состояние**: команды звука, камеры,
  секторов/полигонов, стрельбы (`FIRE_*`) и переходов уровня записываются в
  `State.events`, но поля состояния не меняют (так устроен интерпретатор).
  Трасса показывает их как `event …`, но изменить их «в игре» отладчик не
  может — это не полный движок.
* **`vm.steps` в дампе** — счётчик интерпретатора, а не число исполненных
  инструкций (см. §3).
* **Проверка графа — структурная**: DOT/Mermaid пишутся как текст; валидность
  проверена разбором (все концы рёбер объявлены, счётчики совпадают, порядок
  стабилен) и в self-test, но `graphviz`/`mmdc` в среде нет, поэтому рендер
  картинки не проверялся.
* **Граф — по одному блоку.** Межблочные вызовы (`GENERATE`) — листовые узлы
  `call`; граф всей программы из графов блоков не строится.
* **Только чтение игры.** Ничего не пишется в каталог установки; вывод —
  только `--dot`/`--mermaid` в указанные файлы.

---

## 10. Связь с `sbtasm.py` и `sbtwrite.py`

| инструмент | роль | стык |
|---|---|---|
| `tools/re/vm_exec.py` | прогон блока и сцена (оракул) | `sbtdbg` берёт оттуда `resolve_level`, `load_script`, `select_scripts`, `make_executor`/`GAME_MODES`, но исполняет своим `DbgExecutor` |
| `tools/editor/sbtasm.py` | текст ↔ байты (дизасм/асм, точный рендер) | `sbtdbg` берёт `block_starts`/`block_spans`, чтобы границы подскриптов были те же, что у `disasm --block`; `--block`/`--script off:` — те же смещения |
| `tools/editor/sbtwrite.py` | запись блока в `.sbt` (пересчёт `SCRIPT.m`, сдвиг колонки 2) | отладчик показывает, **что** исполнится; писатель — **как** положить правку в файл |

Рабочий цикл правки: `sbtdbg --list-blocks`/`--graph` → найти блок →
`sbtdbg --run --watch`/`--break-*` понять поведение → `sbtasm disasm --exact`
→ правка текста → `sbtasm --asm` → `sbtwrite --script … --block … --asm …` →
`sbtdbg` на копии `.sbt` (`--game DIR`) ещё раз. Отладчик не меняет файлы.

---

## 11. Воспроизведение и замеры

```sh
cd /home/nooverload/gunmetal-re        # или worktree
python3 tools/editor/sbtdbg.py --self-test
python3 tools/editor/sbtdbg.py --run level0 --script main --trace-limit 10
python3 tools/editor/sbtdbg.py --run dm1 --script main --trace-limit 8
python3 tools/editor/sbtdbg.py --run level0 --script all
python3 tools/editor/sbtdbg.py --run level0 --script off:0xbd8b \
        --source-player 0 --break-offset 0xbdb2 --continue 2
python3 tools/editor/sbtdbg.py --run dm1 --script off:0x2a8d \
        --watch vm.ticks --no-follow
python3 tools/editor/sbtdbg.py --list-blocks level0
python3 tools/editor/sbtdbg.py --graph level0 --block 0x6f92 \
        --dot /tmp/opencode/6f92.dot --mermaid /tmp/opencode/6f92.md
python3 tools/editor/sbtdbg.py --graph dm1 --block 0x2a8d \
        --dot /tmp/opencode/loop.dot --mermaid /tmp/opencode/loop.md
```

Замеры (WSL2, зеркало установки; время зависит от нагрузки):

| команда | результат | время |
|---|---|---:|
| `--self-test` | 26/26 | ≈0,05 с |
| `--run level0 --script main --trace-limit 10` | 24 инструкции, `end` | ≈0,06 с |
| `--run level0 --script all` | 350 блоков, 1 888 инструкций, все `end` | ≈0,6 с |
| `--list-blocks dm1` | 363 блока, 83 990 Б | ≈0,09 с |
| `--graph level0 --block 0x6f92` | 84 узла / 92 ребра, 2 файла | ≈0,1 с |

Ошибки — текстом в stderr и кодом возврата:

```
$ python3 tools/editor/sbtdbg.py --run nosuchlevel; echo rc=$?
error: level not found: nosuchlevel
rc=2
$ python3 tools/editor/sbtdbg.py --run level0 --break-op GENERAT; echo rc=$?
error: unknown --break-op 'GENERAT'; did you mean GENERATE? (names are the command table: GENERATE, ADD_SOURCEPLAYER_CASH, IF ...)
rc=2
$ python3 tools/editor/sbtdbg.py --run level0 --watch player.nope; echo rc=$?
error: unknown --watch 'player.nope'; player fields: playertype, cash, health, shield, ...
rc=2
$ python3 tools/editor/sbtdbg.py --graph level0; echo rc=$?
error: --graph needs --dot FILE and/or --mermaid FILE
rc=2
```

Диагностика интерпретатора (например, само-`GENERATE` без `--no-follow`)
даёт `reason=recursion` и код `1`:

```
$ python3 tools/editor/sbtdbg.py --run dm1 --script off:0x2a8d; echo rc=$?
  reason=recursion steps=10 paths=1 scene=attached=2
...
rc=1
```
