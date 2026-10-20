# Вложенные things: статический рекурсивный обход и исполнение

Документ описывает opt-in API `gm/scene.py` для вещей, которые строятся
командами `ADD_THING*` (условия 1..5) внутри `IF`-веток подсценариев
сущностей, и инструмент `tools/re/nested_things.py`.  Значения по умолчанию
не изменились: `scene.load()` / `scene.things()` дают те же 4 008 вещей, что
и раньше.

## 1. Модель

`SCRIPT` (тип 11) не содержит готового списка объектов: сцена строится
исполнением кода.  У сущности есть запись `(flags, id, offset)`; `GENERATE`
запускает подсценарий по `offset` через `0x457390`.  Внутри подсценария
`ADD_THING*` — это **условие** (`EVAL`, опкод `0xC9`), которое заодно строит
вещь вызовом `0x46d000`.

Компилятор раскладывает ветвление так (проверено по `0x4574db`/`0x45757a` и
по всем 36 уровням, см. `docs/VM_EXEC.md` §1):

```
IF <cond> <size> { then-код ... ELSE <rel> } else-код ... join
```

* `size` считается от dword размера и покрывает **then-ветку вместе с
  инструкцией `ELSE`**; `IF false` прыгает на `else-код`, `IF true` исполняет
  then-ветку, а `ELSE` (последняя инструкция then-ветки) перепрыгивает
  `else-код` на `join`.
* «else-код» физически лежит **после** `IF` и в дереве декодера является
  соседним top-level кодом, а не `body` инструкции `ELSE`.
* `0xFF` завершает подсценарий целиком, поэтому рекурсивный обход
  останавливается на нём на любом уровне вложенности (так же, как старый
  top-level цикл `things()`).

Отсюда три разных множества:

| множество | что это | число |
|---|---|---:|
| top-level | `ADD_THING*` на верхнем уровне подсценария (старое поведение) | **4 008** |
| recursive | top-level + все `ADD_THING*` в then-ветках `IF` (статически, обе ветки) | **7 563** |
| executed | что реально построил VM в заданном контексте (с учётом петель) | 5 019 / **7 617** |

`recursive - top-level = 3 555` — это и есть «потерянные» вещи внутри
`IF`-веток.  `executed` может быть **больше** `recursive`: сайт в цикле
исполняется несколько раз (в полной матрице +54 повторных исполнения,
7 563 + 54 = 7 617).

## 2. API (`gm/scene.py`)

### 2.1. Статический рекурсивный обход

```python
from gm import scene

sc = scene.load("dm1.sbt", execute=True)
print(len(sc.things), len(sc.recursive_things))   # 38, 138
print(sc.nested_counts())
# {'top_level': 38, 'nested': 100, 'total': 138, 'executed': 0, ...}
```

`scene.things(script, sbt_file=None, recursive=False, strict=None)`:

* `recursive=False` — прежнее поведение и прежние 4 008;
* `recursive=True` — обход `IF`-`body` с метаданными и дедупликацией по
  смещению инструкции (`site`), так что две записи сущностей на один блок не
  дают двойного счёта;
* `strict` — при `recursive=True` по умолчанию `True`: битый блок или
  вложенность глубже `MAX_THING_DEPTH` (64) даёт `NestingError`; при
  `strict=False` блок пропускается, как в старом коде.

`Thing` получил дополнительные поля (у старых вызывающих они остаются
default-ными, порядок позиционных аргументов сохранён):

| поле | смысл |
|---|---|
| `source` | id сущности, чей подсценарий создал вещь |
| `path` | цепочка объемлющих `IF`-условий, извне внутрь, напр. `("IF RANDOM",)`; `()` — top-level |
| `site` | смещение инструкции `EVAL ADD_THING*` в коде (уникально) |
| `depth` | `len(path)`; 0 = top-level |
| `via` | `"static"` или `"executed"` |
| `occ` | счётчик исполнения сайта в рамках пути (петли) |

Свойства `Thing.nested` / `Thing.top_level` — просто `depth > 0` / `== 0`.
`Scene.nested_counts()` отдельно считает top-level, nested, сайты и дубли;
`Scene.counts()` **не изменён**.

### 2.2. Исполнение в контексте

```python
ctx = scene.ThingContext(game_mode=1, playertype=2, difficulty=2)
sc = scene.load("level13.sbt", execute=True, context=ctx)
things = sc.nested_things(ctx)          # кэшируется в sc.executed_things
```

* `ThingContext` (frozen dataclass) — `game_mode`, `difficulty`, `players`,
  `playertype`, `source_player`, `god_mode`, `objectives_complete`, `seed`,
  `random_unknown`, `follow_generate`, лимиты исполнителя.
  `objectives_complete` подставляет список `DEFINE_LEVEL_OBJECTIVES` из
  главного блока и все биты целей (иначе условию
  `LEVEL_OBJECTIVES_COMPLETE` нечего проверять в подсценарии сущности).
* `scene.execute_things(script, sbt_file, context, executor=None, path_map=None)`
  прогоняет **каждый** блок сущности (`source_entity` = id), форкает
  недоказуемые `IF` (`on_unknown="both"`), сливает сцены по
  `(site, occurrence)` — ровно тот же merge, что в
  `tools/re/vm_exec.py --corpus`.  Возвращает `ExecutedThings`
  (`things`, `spawns`, `reasons`, `paths`, `blocks`).
* `scene.executed_things(...)` — только список вещей.
* `Scene.nested_things(context=None)` — без контекста отдаёт статический
  рекурсивный список; с контекстом запускает VM и кэширует результат.
* `load(path, execute=False, context=None)` / `from_sbt(...)`:
  `execute=True` заполняет `recursive_things`; `context` дополнительно
  исполняет VM.  Без аргументов результат идентичен прежнему.

У исполненных вещей `via="executed"`, `source` — фактическая сущность-источник,
`path`/`depth` берутся из статической карты сайтов (контекст говорит, какая
ветка сработала; `path` — где сайт синтаксически находится).

## 3. Инструмент `tools/re/nested_things.py`

```bash
python3 tools/re/nested_things.py --self-test        # без файлов игры
python3 tools/re/nested_things.py --check            # 36 уровней, 1 контекст
python3 tools/re/nested_things.py --check --contexts all --random-unknown
python3 tools/re/nested_things.py --check --contexts all --json
python3 tools/re/nested_things.py --level dm1 --contexts all --random-unknown
```

* `--check` — сверка top-level / recursive / executed, дублей, спавнов и
  ожидаемых итогов (4 008 / 7 563 / 7 617 / 391); код возврата 1 при
  расхождении.  Для одного уровня (`--level`) проверяются только
  инварианты, а не корпусные итоги.
* `--json` — построчный отчёт, включая `contexts` (counts на каждый
  контекст), `reasons` и `checks`.
* `--contexts all` — матрица `game_mode 0/1 × playertype 0..3`.
* `--random-unknown` — `RANDOM` форкается (обе ветки), именно этот режим
  даёт 7 617.
* `--difficulty`, `--objectives`, `--source-player`, `--god-mode`,
  `--follow-generate` — дополнительные состояния; `--follow-generate`
  включает переходы `GENERATE` (используется в негативном тесте цикла).
* `--reach` — добавляет два контекста из §4.2 и проверяет, что
  `unreached == 0` на всех уровнях.
* `--self-test` — 15 проверок на синтетическом байт-коде: top-level vs
  then-ветка, `ELSE`/else-код, дедупликация, глубина, OOB, цикл
  `GENERATE`, гейты difficulty/source-player, метаданные исполненных вещей.

Игра открывается только на чтение.

## 4. Доказательства по корпусу

Все 36 `.sbt`, `--seed 0x12345678`:

| метрика | значение |
|---|---:|
| уровней | 36/36 |
| top-level things (`scene.things`) | 4 008 |
| recursive things (`recursive=True`) | 7 563 |
| вложенных (recursive − top-level) | 3 555 |
| статических дублей сайтов | 0 |
| executed, один контекст (campaign, diff 1) | 5 019 |
| executed, матрица `--contexts all --random-unknown` | **7 617** |
| loop-повторов в матрице (executed − recursive) | +54 |
| неисполненных статических сайтов после матрицы | 38 |
| executed, матрица + `--reach` | 7 655 |
| неисполненных сайтов после `--reach` | **0** |
| spawns static / executed | 391 / **391** |
| spawns missing / extra | 0 / 0 |
| не-терминальные причины исполнения | 0 |

Построчная таблица печатается `--check` (36 строк) и доступна в `--json`.
Ни на одном уровне нет дублей статических сайтов; объединение контекстов
дедуплицируется по `(site, occ)`, поэтому один и тот же сайт, исполненный в
нескольких контекстах, считается один раз, а реальный повтор в петле — нет.

### 4.1. Где контекст меняет результат

`--json` даёт `contexts` с counts на каждый из 8 контекстов.  Замеры по
корпусу:

* campaign/diff 1 (один контекст) — 5 019 вещей; этого мало, потому что
  неисполненными остаются then-ветки, зависящие от `RANDOM` (форкается
  только с `--random-unknown`), а также гейты сложности/целей/типа игрока.
* `--random-unknown` + матрица режимов/типов доводит до 7 617.  Внутри
  матрицы **playertype 0..3 не меняет число вещей ни на одном уровне**, а
  `campaign`/`dm-normal` меняет на девяти:

  | уровень | static | campaign × 4 типа | dm-normal × 4 типа |
  |---|---:|---:|---:|
  | level10 | 169 | 171 | 170 |
  | level13 | 267 | 267 | 268 |
  | level14 | 249 | 253 | 243 |
  | level15 | 190 | 191 | 174 |
  | level17 | 299 | 292 | 283 |
  | level22 | 320 | 324 | 306 |
  | level3 | 248 | 247 | 245 |
  | level4 | 181 | 183 | 182 |
  | secrt1 | 175 | 176 | 172 |

  Разница — гейты `DEATHMATCH_GAME`/`IF SOURCE_IS_PLAYER`; счётчик выше
  static там, где петли исполняют сайт повторно (например level10:
  171 = 169 + 2 loop-повтора).
* Полная матрица оставляет **38** статических сайтов неисполненными
  (совпадает с `docs/VM_EXEC.md` §6.3):

  | уровень | сайтов | условие |
  |---|---:|---|
  | level3 | 3 | `GAME_DIFFICULTY_AT_LEAST` |
  | level13 | 1 | `IF SOURCE_IS_PLAYER` → `IF DEATHMATCH_GAME` |
  | level17 | 9 | `GAME_DIFFICULTY_AT_LEAST` (в т.ч. 2 `AT_SOURCE`) и `LEVEL_OBJECTIVES_COMPLETE` |
  | level18 | 15 | `GAME_DIFFICULTY_AT_LEAST` |
  | secrt1 | 3 | `GAME_DIFFICULTY_AT_LEAST` (двойная вложенность) |
  | secrt2 | 6 | `GAME_DIFFICULTY_AT_LEAST` |
  | level15 | 1 | `LEVEL_OBJECTIVES_COMPLETE` |
  | **итого** | **38** | |

  Это не мёртвый код (см. §4.2), а состояния игры, которых нет в базовой
  матрице.

### 4.2. Проверка «достижимых» контекстов

Ко всем 38 сайтам можно дойти, добавив к матрице два контекста:

1. `game_mode=0` (campaign), `difficulty=10`, `objectives_complete=True` —
   снимает гейты сложности и целей (в campaign-ветках);
2. `game_mode=1` (dm-normal), `source_player=0` — снимает
   `IF SOURCE_IS_PLAYER → IF DEATHMATCH_GAME` в level13.

После них неисполненных статических сайтов **0** на всех затронутых
уровнях: `--contexts all --random-unknown --reach` даёт **7 655** вещей
(7 617 + 38 ранее недостижимых сайтов) и `unreached = 0` на всех 36
уровнях; проверка входит в `--check` (`all static sites reached`).
Тот же результат получается вызовом `scene.execute_things` с общей
`path_map`; отдельные контексты доступны в CLI через `--difficulty`,
`--objectives`, `--source-player`, `--contexts all`.  Важно: гейт
`GAME_DIFFICULTY` в этих блоках стоит в **else-коде**
`IF DEATHMATCH_GAME`, поэтому кампанийный режим обязателен — в DM
исполняется then-ветка `ELSE`-прыжка.

## 5. Ограничения

* `path` — **синтаксическая** цепочка `IF`, а не трасса исполнения: по ней
  видно, где сайт лежит, но не какая ветка сработала в конкретном контексте.
  Для ветвления используйте `Scene.nested_things(context)`.
* else-код в декодере — соседний top-level код (см. §1); поэтому он входит
  в top-level 4 008, если содержит `ADD_THING*`.  «Вложенными» считаются
  только then-ветки; это соответствует устройству компилятора, а не
  семантике «условно исполняемый код».
* Исполнитель — модель (`docs/VM_EXEC.md` §7): 117 event-команд не
  моделируются, `RANDOM` детерминирован seed-ом, `on_unknown="both"`
  форкает только недоказуемые условия, `GENERATE` по умолчанию не
  преследуется (вещи строятся из блоков таблицы сущностей).
* `MAX_THING_DEPTH = 64` (в корпусе максимум 5).  Более глубокая вложенность
  — `NestingError`, а не молчаливое усечение.
* Дедупликация статики — по `site`; если один блок назван двумя сущностями,
  вещь учитывается один раз (у исполнения — по `(site, occ)`).
* `things()` читает только записи `THING_TYPES`/`SOB_TABLE` типа 4/20;
  `ADD_THING_*` с невалидным `G` (0xFFFFFFFF) остаются с пустым именем, как
  и раньше.
* API не отменяет последующие команды (`DELETE_LOCATION`, `REMOVE_*`):
  это статический/исполняемый список создания, а не итоговое состояние
  уровня после всех событий.

## CLI

`python3 gm.py scene LEVEL.sbt --nested` добавляет к обычной сводке строку
`top-level / nested / static sites / executed` (opt-in; без флага counts не
меняются). Для фактического исполнения контекста используйте
`tools/re/nested_things.py --contexts all`.
