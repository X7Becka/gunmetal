# Трасса ВМ: C++ против Python (`M5.3.2`) — закрыто

Проверка «один и тот же байт-код даёт одну и ту же трассу в C++ и Python»
пройдена на всех 36 уровнях: **328 524 события совпадают побайтово**
(pc, глубина, команда, все операнды), 15 061 блок, 0 расхождений.

Инструмент: `tools/re/vm_trace_check.py`; C++-дампер:
`engine/tests/vm_trace_dump.cpp`.

## Как запускать

```bash
python3 tools/re/vm_trace_check.py --self-test          # 6 проверок анализатора
python3 tools/re/vm_trace_check.py --level level0       # главный блок
python3 tools/re/vm_trace_check.py --level dm1 --blocks all
python3 tools/re/vm_trace_check.py --level level1 --blocks 0,3,17 --dump-py
ctest --test-dir build/sandbox -R vm_trace               # 20 блоков level0+dm1
```

Дамп печатается в языконезависимом виде, поэтому сравниваются **точные**
значения, а не форматирование:

```
block 174 eid=-30186 off=0xa5e6 paths=7
path 0 reason=end steps=7 pc=0xa5f5
ev 0xa5e6 0 IF 0
ev 0xa5f0 0 SET_SOURCEPOLY_SWITCH_EVENT 1 i:65535
ev 0xa5f5 0 END 0
```

Аргумент — `i:<u32>`, `d:<16 hex>` (биты IEEE-754) или `s:<hex>` (строка
побайтово). Порядок путей внутри блока — деталь реализации (очередь
frontier), поэтому пути сравниваются как мультимножества.

Две группы команд отдают полезную нагрузку не в `args`, и их аргументы
исключены из сравнения (декодер этих таблиц уже запинен в `sbtvm`):
`DEFINE_WEAPONS/AMMO/PLAYER_TYPES/EQUIPMENT/LEVEL_OBJECTIVES`,
`SOURCEPLAYER_PATH`, `ADD_LIFT`, `ELSE` (`("rel", target)` в Python,
`insn.target` в C++).

## Результат

| уровень | блоков | путей | событий |
|---|---:|---:|---:|
| `level0` | 350 | 525 | 6 688 |
| `dm1` | 363 | 538 | 7 423 |
| `level1` | 360 | 543 | 7 408 |
| **36 уровней** | **15 061** | — | **328 524** |

CTest `vm_trace`/`vm_trace_dm1` держат 20 блоков `level0` (65 путей,
1 670 событий) и 20 блоков `dm1` (41 путь, 656 событий), чтобы регрессия
трассы ловилась на каждом прогоне.

## Что было не так

До правки расходились 13 блоков (4 в `level0`, 7 в `dm1`, 2 в `level1`):
Python исполнял на 20 путей больше там, где C++ шёл в `end`.

Причина одна — `gm/sbtvm_exec.py::_h_generate` брал `st.entities.get(gid)`
(словарь, то есть **последнюю** строку с этим id) и запускал её, не глядя
на флаги. `generate_runner` `0x45fa00` устроен иначе
(`docs/ENTITY_FIELDS.md` §«Дубли id разрешены и все исполняются»):

* фильтр по биту источника: `0x45fa74…0x45fa93` — `TEST EAX,EDX` для
  `param_8 & 0xffff < 8`, иначе `TEST AH,0x1` (`0x100`); строка без бита
  не исполняется;
* строка с `0x200` (one-shot) гасит `0x1ff` **до** вызова `0x457390`
  (`0x45fa9e`), поэтому самовызов не входит повторно;
* **все** строки с этим id исполняются последовательно (`0x45fb09…0x45fb1c`),
  и `ENABLE`/`DISABLE` из первой строки влияет на проверку второй
  (`0x45fa83` читает колонку в момент запуска строки).

## Правка (Python приведён к движку)

`gm/sbtvm_exec.py`:

* `State.entity_rows` — все строки по id в табличном порядке;
  `State.entities` — первая строка (как C++ `VmState::entity(id)`);
* `_add_entity` наполняет оба представления (и `state_from_script`, и
  синтетика `_run_bytes`);
* `ENABLE`/`DISABLE`/`SOURCEPLAYER_ENABLE`/`SOURCEPLAYER_DISABLE` меняют
  **все** строки id (`0x457c67` идёт по всей колонке без `break`);
* `_next_row(st, gid, cursor)` — выбор следующей подходящей строки
  (фильтр бита, one-shot-сброс), `_leave_frame` — возврат из подскрипта
  с запуском следующей строки того же id;
* кадр стека теперь `(return_pc, source, generate_id, row_cursor, site)` —
  как `VmStackFrame` в C++; `END` и `EXIT_EVENT` идут через `_leave_frame`.

Самопроверки: `tools/re/vm_exec.py --selftest` 17/17 (внутри проверки
«recursion detected» — четыре случая: self-GENERATE → recursion, one-shot →
`end`, DISABLED → `end`, дубли → обе локации по порядку);
`tools/re/nested_things.py --self-test` 15/15 (`FakeScript` принимает
`(id, offset, flags)`).

## Проверки после правки

| проверка | результат |
|---|---|
| `vm_trace_check.py --blocks all`, все 36 `.sbt` | 36/36 уровней идентичны, 328 524 события |
| `vm_scene_check.py --summary` | сцена 36/36, objects 4 470/461/101/1 173/555/1 779/3 964/45/391, things 7 655/7 655 |
| `vm_exec.py --corpus --compare` | 36/36 блоков до `END`, код 0 |
| `regress.py --mirror` | 114/114 |
| `ctest -L quick` | всё, кроме `controls` (внешний `KEYCFG/*.key`, не связан) |
