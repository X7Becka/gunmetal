# C++-исполнение SCRIPT-VM (`vm_exec.h` / `vm_exec.cpp`)

Статус: **порт эталонного Python-исполнителя `gm/sbtvm_exec.py`**, чтение
игры, без запуска `gundx.exe`. Код: `engine/src/res/vm_exec.{h,cpp}`,
тесты `engine/tests/test_vm_exec.cpp`. Эталон и разбор дизассемблера —
`docs/VM_EXEC.md` (§1 цикл, §4 таблица условий), декодер — `sbtvm.*`.

Цель порта — не «похожее» поведение, а **побайтовое совпадение с Python**:
тест сравнивает число блоков, путей, инструкций и все категории сцены с
`gm/sbtvm_exec.py` на реальных 36 уровнях.

## 1. Модель исполнения

Как и в движке (`0x457390`), здесь нет стека операндов и арифметических
опкодов. Цикл:

```cpp
pc = 0;
for (;;) {
    op = code[pc++];
    if (op == 0xff) {                       // END / EXIT_EVENT
        if (!stack.empty()) { pc = return_pc; continue; }
        return End;
    }
    if (op == 0xc9) { eval_condition(); pc = next; continue; }  // EVAL
    if (op == 199) {                        // IF
        r = eval_condition();
        pc = r ? body_start : body_end;     // true: skip the sizing dword
        continue;
    }
    if (op == 200) { pc = target; continue; } // ELSE: u32 jump forward
    dispatch(op);                           // 0x45c500
}
```

Ключевое следствие для `IF/ELSE`: `ELSE` (200) стоит **последней
инструкцией then-ветки**, а не разделителем. Поэтому ложное условие
попадает ровно на код после then (`body_end`), а истинное — на `body_start`;
`ELSE` в конце then перепрыгивает else-код. Это воспроизведено в C++ и
проверено синтетикой (`enc_if` в тесте строит байты так же, как компилятор).

Состояние (`gm::VmState`) — прямой аналог `State` из Python:

| поле C++ | смысл |
|---|---|
| `code`, `pc`, `steps` | буфер SCRIPT и текущий PC; счётчик инструкций |
| `entities[id] = {flags, offset}` | три колонки таблицы сущностей (`flags/id/offset`) |
| `stack`, `active` | стек подскриптов `GENERATE` (return PC, source) и активные offset |
| `players[i]` | type/behaviour/key/weapon/equipment/extra биты, cash, health, shield, ammo, equipment_active, индексы, salvage/morale/secrets |
| `source_player`, `source_entity`, `source_alt` | контекст источника (`SOURCE_CLEAR`, `SOURCE_SWAP`) |
| `game_mode`, `mp_flags`, `player_count`, `difficulty`, `level_number`, `god_mode` | контекст игры |
| `objectives`, `objectives_bits`, `defs`, `table_defs`, `path` | `DEFINE_*`, цели, путь |
| `scene` | lights/locations/items/named_items/attached/sounds/sprites/things/doors/switches/lifts/spawns |
| `events` | всё исполненное, что не влияет на модель (звук/экран/камера/мир) |
| `clock`, `rng`, `occ`, `trace` | виртуальное время `SLEEP`, ГПСЧ, счётчик повторных исполнений точки, трасса |

Точка входа подскрипта — `vm_state_from_level`, которая читает таблицу
сущностей ровно как `gm.scene.entities`/`gm.scene.py`: `n`, затем три
колонки по `n` u32. Порядок строк сохраняется (`VmState::order`), дубликаты
id в таблице не склеиваются при обходе блоков.

## 2. Команды и условия: что покрыто

Всего 199 опкодов: 192 именованных + 7 структурных. Классификация взята из
Python без изменений (`vm_command_status`):

| статус | опкодов | что делает C++ |
|---|---:|---|
| `structural` | 7 | читает таблицы `DEFINE_*`, цели, `SOURCEPLAYER_PATH`, `ADD_LIFT` |
| `flow` | 8 | `EXIT_EVENT`, `EXIT_PROGRAM`, `SLEEP`, `GENERATE`, переходы уровня |
| `scene` | 14 | создаёт объект с абсолютной позицией (реализовано) |
| `attached` | 10 | создаёт объект «у источника»: фиксируются имя, аргументы, source (мировой позиции нет) |
| `state` | 43 | меняет поля модели (реализовано) |
| `event` | 117 | фиксируется в `events`, на модель не применяется (как в Python) |
| `unsupported` | 0 | в поставке не осталось; путь кода есть (`unsupported-command`) |

Условия (вторая ВМ `0x45d120`, таблица `0x45f2ac`, 59 обработчиков):

* **action** (21 id: `ADD_THING*`, `FIRE_*`, `SOURCE_SWAP`, телепорты,
  `CUSTOM_COMMAND_*`, `ADD_OTHER`) — создают вещь/эффект; `ADD_THING*` и
  `SOURCE_SWAP` дают `true`, остальные `open` (`nullopt`);
* **value** (26 id) — `difficulty`, цели, номер уровня, DM/CTF/team,
  `PLAYERS_AT_LEAST`, `RANDOM`, `GOD_MODE`, `KEY/WEAPON_BITS_SET`,
  `AMMO_AT_LEAST`, `CASH_AT_LEAST`, `SOURCEPLAYER_IS`, `LIFE/SHIELD_*`,
  `SOURCEPLAYER_TYPE`;
* **open** (12 id: `NEAREST_OBSTRUCTION*`, `SOURCE_FROM_GROUND`,
  `SOURCEPLAYER_AMMO_FULL`, `EXTRA_BITS_SET`, `HAS_WEAPON_FOR_AMMO`,
  `SOURCEPLAYER_THING_TYPE`, `SOURCE_UNDERWATER`,
  `SOURCEPLAYER_UNDERWATER`, `SOURCEPLAYER/SOURCESECTOR_FLAGS`,
  `SOURCEPOLY_HANDLE`) — честно `nullopt`, политика `on_unknown`.

Политика неопределённости — как в Python: `stop | false | true | both`
(по умолчанию `both`: состояние клонируется, else-ветка кладётся во frontier,
then исполняется сразу). `max_paths` ограничивает frontier.

Конкретная семантика обработчиков (все — из `docs/VM_EXEC.md` §3.1):
`GENERATE` переключает дверь (`toggles += 1`) и вызывает подскрипт
сущности; `ENABLE/DISABLE` — биты `0x1ff`; `SOURCEPLAYER_ENABLE/DISABLE` —
бит `1<<(9+src)` (или `0x100`); `SLEEP` — `clock += operand`;
`SET_PLAYER_START`/`SET_DEATHMATCH_START` — spawn; `ADD_LIGHT*`, `ADD_LOCATION`,
`ADD_ITEM*`, `ADD_SOUND_SOURCE`, `ADD_SPRITE`, `ADD_DOOR`, `ADD_SWITCH`,
`ADD_LIFT`, `DELETE_LOCATION` — объекты сцены; `ADD_SOURCEPLAYER_*`,
`HEAL/REPAIR`, `SOURCEPLAYER_KILL`, `SOURCE_CLEAR`, `DEFINE_SHIELDS`,
`DEFINE_AMMO` (кламп патронов), биты игрока — поля модели.

Неизвестный опкод **не пропускается молча**: декодер помечает инструкцию
`VmInsnKind::Unknown`, исполнитель останавливает путь с `unknown-opcode` и
точным PC; `unsupported-command` — для команды без обработчика.

## 3. Лимиты и безопасность

| предел | значение | поведение |
|---|---|---|
| `max_steps` | 200 000 | `step-limit` |
| `max_time` | 5 с на `run` (0 = выкл.) | `deadline` |
| `max_paths` | 64 | else-форк не создаётся (`path-limit`-диагностика в note) |
| глубина `GENERATE` | 32 | `stack-overflow` |
| активный offset | — | повторный вход = `recursion` |
| таблицы `DEFINE_*` | 31/31/32/31 записей | `bad-operand` |
| цели / путь / стопы лифта | 30 / 32 / 31 | `bad-operand` |
| события / трасса | 200 000 / `trace_limit` | обрезаются |

Ридер операндов проверяет границы на каждом чтении (`need`), строки
обязаны быть NUL-терминированы, IF-размер обязан покрывать dword и не
выходить за код, `ELSE`-цель проверяется перед переходом, PC после
обработчика — `pc <= code.size()`. Доступ к операндам идёт через
`std::vector::at`, поэтому битый набор операндов не даёт OOB (исключение
переводится в `bad-operand`). Сцена и события ограничены шагами/лимитами,
никаких аллокаций по операндам без проверки счётчика.

Детерминизм: ГПСЧ — LCG `rng = rng*1103515245 + 12345 (mod 2^31)` и
`(rng & 0xfff)/4095` (как Python, сид `0x12345678`); тесты запускают VM с
`max_time = 0`, поэтому результат не зависит от времени.

## 4. Сверка с Python

Тест (`gm_vm_tests`) считает блоки так же, как `tools/re/vm_exec.py
--script all`: уникальные offset из таблицы сущностей в порядке таблицы,
один контекст (campaign, difficulty 1, seed `0x12345678`,
`follow_generate=false`, `on_unknown=both`). Голдены сняты с Python.

### 4.1. Три уровня (по умолчанию)

| метрика | level0 | dm1 | level1 |
|---|---:|---:|---:|
| блоков / путей | 350 / 468 | 363 / 479 | 438 / 563 |
| инструкций (шагов) | 2 821 | 3 225 | 3 848 |
| lights | 29 | 60 | 90 |
| locations | 10 | 8 | 48 |
| items (безымянные) | 2 | 1 | 10 |
| named_items | 1 | 125 | 114 |
| attached | 135 | 182 | 175 |
| sounds | 6 | 21 | 49 |
| sprites | 4 | 0 | 0 |
| things | 51 | 61 | 86 |
| doors | 25 | 61 | 33 |
| switches | 24 | 129 | 88 |
| lifts | 0 | 0 | 1 |
| spawns | 8 | 13 | 16 |

Дополнительно для level0 сверяются 8 позиций `SET_PLAYER_START`
(index/x/y/z) — то есть не только счётчики, но и разбор операндов.
Все пути заканчиваются `end`/`level-transition` (в dm1 и level1 ровно один
`level-transition` — `NEXT_LEVEL`/`NEW_LEVEL` в подскрипте), как в Python.

### 4.2. Весь корпус (`--full`)

| метрика | Python | C++ |
|---|---:|---:|
| уровней / блоков | 36 / 15 061 | 36 / 15 061 |
| путей | 19 988 | 19 988 |
| декодированных инструкций (шагов) | 138 980 | 138 980 |
| исполненных команд | **78 421** | **78 421** |
| исполненных условий | 19 535 | 19 535 |
| различных команд / условий | 156 / 42 | 156 / 42 |
| lights / locations / items | 4 470 / 461 / 100 | совпадает |
| named_items / attached | 4 304 / 6 121 | совпадает |
| sounds / sprites / things | 1 172 / 551 / 5 019 | совпадает |
| doors / switches / lifts | 1 779 / 3 964 / 45 | совпадает |
| spawns | 391 | 391 |

Число **78 421** из `docs/VM_EXEC.md` — это исполненные *команды*
(без `IF`/`ELSE`/`EVAL`/структурных/`END`), а не все шаги; C++ воспроизводит
именно его. Полное число декодированных инструкций — 138 980. Оба числа
согласованы с текущим Python-исполнителем.

### 4.3. Главные скрипты

`gm_vm_tests` прогоняет offset 0 каждого из 36 уровней с
`follow_generate=true` (как Python `--script main`): **36/36 доходят до
`END`**, число шагов совпадает с голенами (16–36 на уровень; на двух
уровнях Python тоже даёт форк по неизвестному условию, поэтому число путей
может быть 1–2).

## 5. Негативы и границы

Синтетические тесты (`test_vm_exec.cpp`, без файлов игры) покрывают:

* `IF/ELSE` (then/else), вложенный `IF`, `IF` с пустым then (`size == 4`);
* сравнение (`SOURCEPLAYER_CASH_AT_LEAST` после `ADD_SOURCEPLAYER_CASH`);
* неизвестное условие: `stop` → `unknown-condition`, `both` → 2 пути;
* неизвестный опкод `0xfe` → `unknown-opcode` с PC, не молчаливый пропуск;
* OOB PC (`ELSE` за конец блока) → `bad-pc`;
* битый операнд: незавершённая строка, обрезанный dword → `bad-operand`;
* рекурсия `GENERATE` себя → `recursion`;
* переполнение стека (цепочка из 40 сущностей) → `stack-overflow`;
* `max_steps` → `step-limit`;
* операнды `SET_PLAYER_START`, `ADD_THING_EX` внутри `EVAL`, `DISABLE`;
* декодер: неизвестный опкод, обрезанный `IF`, PC за кодом.

Осознанные ограничения (те же, что у эталона, плюс отмеченное):

1. **`event`-команды (117) не моделируются.** Это звук, экран, камера,
   фильмы, мутации секторов/полигонов и часть `SOURCEPLAYER_*`. Для PC и
   сцены они безопасны, но условие, читающее изменённое ими поле, может
   быть посчитано по устаревшему состоянию (в Python — так же).
2. **`attached` (10 команд)** фиксируют факт и аргументы без мировой
   позиции: её даёт только рантайм-источник.
3. **`DEFINE_*`**: интерпретируются `DEFINE_AMMO` (макс. патронов) и
   `DEFINE_SHIELDS` (max shield); `DEFINE_PLAYER_TYPES`/`WEAPONS`/`EQUIPMENT`
   сохраняются, но в модель здоровья/урона не разворачиваются.
4. **`RANDOM`** — формула доказана, сам генератор `0x480e30` не восстановлен;
   используется LCG с сидом (как в Python). `RANDOM_FRAME_BALANCED` — та же
   гипотеза.
5. **`ADD_NAMED_ITEM(_EX)`** — id/позиция берутся по порядку операндов
   (гипотеза; `_EX` в движке сначала проверяет `a1/a2` по `0x83d62c`).
6. **`GENERATE_FOR_CHILDREN/PARENT`** отнесены к `event`: подскрипты они не
   запускают (`0x4575d3`/`0x4576b1`).
7. **API `run(start, source_entity)`** выставляет источник только если
   аргумент передан. В Python `run(source_entity=None)` явно сбрасывает
   источник; для состояний из `vm_state_from_level` источник и так пуст,
   поэтому корпусные результаты идентичны. Это единственное отличие
   интерфейса, а не модели.
8. **Время.** В тестах `max_time = 0`; в остальном по умолчанию 5 с на
   `run` (диагностика `deadline`). В корпусе пределы не срабатывают.

## 6. Что осталось

* Довести `event`-команды до состояния модели (сектора/полигоны/звук) —
  тогда условия, читающие эти поля, перестанут быть `open`.
* Восстановить настоящий ГПСЧ `0x480e30`, чтобы `RANDOM` совпадал с игрой,
  а не только с Python-моделью.
* Проверить `ADD_NAMED_ITEM_EX` и `DEFINE_PLAYER_TYPES` по обработчикам
  (сейчас — гипотезы/сохранение).
* Мировые позиции `attached`-объектов (нужен рантайм-источник) и резолв
  type/model вещей (сейчас это делает `scene.cpp`, не VM).
* Матрица контекстов `--contexts all --random-unknown` (8 контекстов) в C++
  не гоняется: тест фиксирует один детерминированный контекст. Это
  расширение теста, а не модели.

## 7. Сборка и запуск

```
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_vm_tests -j 32
ctest --test-dir build/sandbox -R vm --output-on-failure   # vm + vm_full (+ sbtvm)
./build/sandbox/engine/tests/gm_vm_tests --full            # 15 061 блок
python3 regress.py                                         # без изменений
```

`vm` (quick) — level0/dm1/level1 + 36 главных скриптов + синтетика (~2 с);
`vm_full` — весь корпус и агрегаты (~10 с в Debug).
