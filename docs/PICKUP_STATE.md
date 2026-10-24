# PICKUP_STATE — правила подбора предметов в движке (`M6.2.6`, срез 3)

**Статус:** чистый модуль правил + тест на синтетике и реальных
`level0.sbt`/`dm1.sbt`. Файлы: `engine/src/game/pickup.h/.cpp`,
`engine/tests/test_pickup.cpp` (CTest `pickup`, label `quick`, **256 проверок**,
0.45 с). RE-канон — `docs/PICKUP.md` (далее `P`), опкоды — `docs/VM_EXEC.md`
(`V`); здесь только перенос в C++ и явное разделение «доказано / модель».

Смежные каноны, которые не пересказываются: `docs/DAMAGE.md` §0 (`D`),
`docs/ECONOMY.md` (`E`), `docs/CHASSIS.md` §3–4, `docs/CAMPAIGN_FLOW.md` §5
(`CF`), `docs/ECONOMY_STATE.md`, `docs/EQUIPMENT_STATE.md`.

## 0. Итог

* Подбор в оригинале — **радиус-скан**, а не касание: физика игрока каждый тик
  зовёт `0x469770` (`0x445165`), тот перебирает 300 слотов и сравнивает
  расстояние с радиусом самого предмета `+0x4c` (`P` §0, §4). Награду выдаёт
  **под-скрипт сущности**, а не движок (`P` §0).
* Этот модуль моделирует именно награду: одну вещь → один инвентарь. Он не
  сканирует радиус, не постит событие, не удаляет предмет (это op 39
  `REMOVE_SOURCE_ITEM`, `P` §4) и не респавнит его (`P` §7).
* Броня — единственная шкала «здоровья» (`D` §0.1); `HEAL_SOURCEPLAYER`
  (op 112, `0x45af95`) прибавляет и зажимает по `armor_max` (`0x45b02a`).
  Поэтому `PickupKind::health` — алиас брони, отдельной шкалы HP нет.
* Щиты — 4 плиты `+0x3250..+0x325c` с одним `shield_max` (`D` §0.2, `P` §8).
  `REPAIR_SOURCEPLAYER` (op 113, `0x45b13d`) с битом флага 1 раздаёт по одному
  очку в фиксированной ротации front→right→back→left, пропуская полные плиты
  (`0x45b177..0x45b1bf`, `ebx` считает полные, цикл до `cmp ebx,4`). Модуль
  реализует эту ветку; вторая (без бита) прибавляет всё число к каждой плите
  и зажимает по максимуму (`0x45b1c3..0x45b1f5`).
* Оружие/экипировка/ключи — битовые маски: op 108/111/106 делают `|=` одного
  бита (`V`), поэтому повтор — доказанный no-op, а не ошибка. Модуль называет
  это `rejected_duplicate`.
* Патроны — одно i32 на тип `0x81c2dc + 4*type`; op 109 прибавляет и зажимает
  `[0, max из DEFINE_AMMO]` (`V`). Повторный подбор того же типа **складывается**
  и дубликатом не является.
* Деньги — одно поле; op 114 прибавляет, снизу зажимает в 0 (`E` §2.1).
* Секрет — счётчик игрока `+0x33cc`; op 95 прибавляет ровно **единицу** и
  предела не имеет: tally может превысить `SET_LEVEL_SECRETS` (`CF` §5,
  случаи `secrt2`/`temple`).
* Масса — **модель**. `E` §6 доказывает, что лимит шасси `+0x3338` покупку не
  ограничивает: единственный читатель — штраф скорости `0x4525e7`. Лимит в
  модуле опционален и по умолчанию **выключен** (`kNoMassLimit = -1`).

## 1. Что доказано (и откуда)

| факт | источник |
|---|---|
| подбор — радиус-скан 300 слотов, триггер `+0x44`, радиус `+0x4c` | `P` §0, §4; `0x469770` ← `0x445165` |
| награда — под-скрипт: `ADD_SOURCEPLAYER_CASH/SALVAGE`, `HEAL_SOURCEPLAYER`, `ADD_SOURCEPLAYER_AMMO`, `SOURCEPLAYER_INFECT`, затем `REMOVE_SOURCE_ITEM` | `P` §0, §6 |
| броня — HP, `armor +0x3248`, `armor_max +0x3244`, сотые; смерть при `armor <= 0` | `D` §0.1 |
| `HEAL_SOURCEPLAYER` (op 112): `armor += arg1`, кламп по `armor_max`; отрицательный arg → `damage_player` | `V` (op 112), `0x45b014..0x45b02a`, `0x45b0a9` |
| щиты — 4 плиты `+0x3250` с одним `shield_max +0x324c`, сотые | `D` §0.2, `P` §8/§9 |
| `REPAIR_SOURCEPLAYER` (op 113): ротация по 1 очку на плиту, пропуск полных; вторая ветка — всем плитам сразу | `0x45b177..0x45b1bf`, `0x45b1c3..0x45b1f5` |
| `ADD_SOURCEPLAYER_AMMO` (op 109): `ammo[type] += delta`, кламп `[0, max]` | `V` (op 109), `0x459ca6` |
| оружие/экипировка/ключи — `|=` бита (op 108/111/106), повтор идемпотентен | `V`, `0x45bbd7`/`0x45badb`/`0x45bb50` |
| `ADD_SOURCEPLAYER_CASH` (op 114): `cash += arg`, снизу кламп в 0 | `E` §2.1, `0x459d99` |
| секрет: tally `+0x33cc`, op 95 `+= 1`, без предела; сброс на старте уровня | `CF` §5, `0x4536b0`/`0x45b978` |
| `DEFINE_SHIELDS` = `(50, 0)` во всех 36 уровнях: цена 100 щитов и вес 100 щитов, **не** предел плиты | `P` §9, `E` §3 |
| `DEFINE_REGENERATION_MARKER` = `(19, 3000.0, 50.0)` во всех 36 уровнях; двойные поля мертвы | `P` §7 |
| корпус: 36 уровней, 1138 прямых записей (`52`×401, `53`×17, `54`×636, `55`×84), 448 декор (`trigger 0`, радиус 0), 690 с триггером | `P` §6 |
| `DEFINE_VIRUS`: `L0 = 16`, `V = 28`, `P = 10`, `D = 0.03` во всех уровнях; `T` свой у уровня | `P` §8 |
| шасси: `op2` (щиты) и `op3` (`armor_max`) хранятся ×100; `armor_max = 10000` у всех | `docs/CHASSIS.md` §3–4, `D` §0.1 |
| в корпусе 4 ветки `DEFINE_PLAYER_TYPES` по 4 строки; LIGHT/MEDIUM/STEALTH/HEAVY = щиты 80/130/90/200 | `docs/CHASSIS.md` §4 |

Данные в тесте **читаются**, а не пересказываются: `gm::load_level` →
`gm::decode_sbt_script` → рекурсивный обход тел `IF/ELSE` →
`ADD_ITEM*`/`ADD_RANDOM_AMMO_ITEM_AT_SOURCE`/`ADD_NAMED_ITEM*`,
`DEFINE_SHIELDS`/`DEFINE_REGENERATION_MARKER`/`DEFINE_VIRUS`/
`DEFINE_PLAYER_TYPES`. Сверка идёт по числам `level0`/`dm1`, поэтому дрейф
постава валит тест.

## 2. Модель

`namespace gm` (имён `PickupRules`/`PickupItem`/`PlayerInventoryModel` в
движке раньше не было — проверено `grep`).

```cpp
enum class PickupKind { armor, shield, ammo, weapon, equipment,
                        health, cash, secret, key, unknown };
struct PickupItem { PickupKind kind; int id; int amount; int mass; };

struct PlayerInventoryModel {
    int armor, armor_max;          // +0x3248, +0x3244 (сотые)
    int shield_max;                // +0x324c — добавлено: 4 плиты делят один max
    std::array<int,4> shields;     // +0x3250..+0x325c
    int cash;                      // +0x332c
    int mass, mass_limit;          // модель; -1 = без лимита
    std::vector<int> weapons;      // id, уникальные, по возрастанию
    std::set<int> equipment;       // op 111
    std::set<int> keys;            // op 106 — добавлено
    std::vector<int> ammo;         // патроны, индекс = тип
    int ammo_max;                  // модель: предел на тип, 0 = без предела
    int secrets, secrets_declared; // +0x33cc, SET_LEVEL_SECRETS
};

enum class PickupResult { taken, already_full, rejected_mass,
                          rejected_duplicate, rejected_unknown };

class PickupRules {
  PickupResult take(const PickupItem&, PlayerInventoryModel&,
                    std::string* message = nullptr);
  const PickupStats& stats() const;
  void reset_stats();
  int  pickup_mass_total() const;                          // Σ mass taken
  static int pickup_mass_total(const std::vector<PickupItem>&);
  const std::string& last_message() const;
};

int distribute_shields(std::array<int,4>&, int max, int amount);
```

Сверх полей из задания добавлены `shield_max`, `keys`, `ammo`/`ammo_max`,
`secrets`/`secrets_declared` — без них доказанные правила (общий максимум
плит, отдельные биты ключей, кламп патронов, счётчик секретов) не выразить.
Обязательные поля задания сохранены и идут первыми.

### 2.1. Порядок проверок и атомарность

```
kind != unknown -> id >= 0 -> amount > 0 -> mass >= 0
  -> no-op вида (already_full / rejected_duplicate)
  -> модельный лимит массы (rejected_mass)
  -> изменение инвентаря
```

Любой отказ оставляет инвентарь без изменений, пишет текст в
`*message`/`last_message()` и увеличивает ровно один счётчик.
`PickupStats` считает `taken`/`already_full`/`rejected_mass`/
`rejected_duplicate`/`rejected_unknown`, `taken_by_kind[10]`, `mass_taken` и
`calls()`. Дубликат проверяется **до** массы: повтор оружия с массой 1000 при
лимите 10 не съедает массу и возвращает `rejected_duplicate`.

### 2.2. Правила по видам

| вид | что делает модуль | результат на границе |
|---|---|---|
| `armor`, `health` | `armor = min(armor + amount, armor_max)` (op 112) | `armor >= armor_max` → `already_full` |
| `shield` | ротация 0→1→2→3 по 1 очку до `shield_max`, пропуск полных (op 113) | все 4 полны (или `shield_max <= 0`) → `already_full`; остаток сверх ёмкости теряется, как в `0x45b1b0` |
| `ammo` | `ammo[id] += min(amount, limit - current)` (op 109) | `ammo_max > 0 && current >= ammo_max` → `already_full`; повтор **складывается** |
| `weapon` | id добавляется в `weapons` один раз (op 108) | повтор → `rejected_duplicate` (доказанный no-op) |
| `equipment` | id в `equipment` (op 111) | повтор → `rejected_duplicate` |
| `key` | id в `keys` (op 106) | повтор → `rejected_duplicate` |
| `cash` | `cash += amount` (op 114) | переполнение `int` → `rejected_unknown` (модель) |
| `secret` | `secrets += 1`, `amount` — не счётчик (op 95) | предела нет: `secrets` может превысить `secrets_declared` |
| `unknown` | — | `rejected_unknown` |

Масса вещи (`PickupItem::mass`) прибавляется к `mass` только при `taken`.
У записи `ITEM` поля массы нет (`P` §2.1), поэтому это вход вызывающего.

## 3. Числа теста (`gm_pickup_tests`)

Синтетика (печатается тестом; `armor_max = 10000`, `shield_max = 8000`,
как у LIGHT):

```text
armor +400 -> 400/10000; +9700 -> 10000 (кламп); +100 -> already_full
health 900 при armor 9500 -> 10000 (та же шкала, D §0.1); ещё -> already_full
shield +100 на 0/0/0/0 -> 25/25/25/25
shield +100 ещё       -> 50/50/50/50
shield +31800         -> 8000/8000/8000/8000 (ёмкость), далее already_full
shield +100 на 8000/7900/0/0 -> 8000/7934/33/33 (полная плита пропущена)
shield +100 при одном свободном очке -> +1 (ёмкость ограничивает)
shield_max 0 -> already_full, distribute_shields(...,0,100) = 0
weapon 7 -> 1 шт.; повтор -> rejected_duplicate; weapon 3 -> 3/7
equipment 4 и key 2: добавление, повтор -> rejected_duplicate
ammo type 0: 100 -> 200 (складывается), +4900 -> 5000 (кламп), +1 -> already_full
ammo type 3: +1000, вектор дорос до 4 типов; ammo_max 0 -> 2000 без предела
масса: без лимита weapon 600 -> mass 600 (правило E §6)
лимит 500: mass 400 + 200 -> rejected_mass (mass 400); 400 + 100 -> 500 (граница);
           +1 -> rejected_mass; взято 2, отказов по массе 2
дубликат оружия с mass 1000 при лимите 10 -> rejected_duplicate (проверен раньше массы)
cash 2000 + 500 -> 2500; cash INT_MAX + 1 -> rejected_unknown
secret amount 5 -> tally +1; 4 из 3 объявленных — всё ещё taken (op 95 без предела)
негативы: пустой item, unknown kind, id -1, amount 0/-5, mass -1, nullptr message
          -> 8 отказов rejected_unknown, инвентарь не изменён
mass_total(batch 600+45+0+(-5)) = 645; после take() model=645, rules=645
детерминизм: два прогона идентичны (63 байта)
```

Данные (`level0` / `dm1`, вывод теста):

```text
level0 items=41 (52:10 53:0 54:16 55:2 177:12 185:0 186:1) trigger 0/non0=9/19 radius 0/non0=9/19
dm1    items=170 (52:10 53:1 54:22 55:0 177:12 185:1 186:124) trigger 0/non0=15/18 radius 0/non0=15/18
shields (50, 0); marker (19, 3000.0, 50.0); virus (16, T=105/199, 12, 28, 10, 0.03)
chassis: LIGHT 10000/8000, MEDIUM 10000/13000, STEALTH 10000/9000, HEAVY 10000/20000
LIGHT caps: armor +2500 -> 2500/10000, shield +400 -> 100/100/100/100
```

Корпус (`P` §6, константы теста): 36 уровней, 1138 прямых записей
(`52:401 53:17 54:636 55:84`), декор 448, с триггером 690, `185:53`,
`186:4252`, `177:440`, 300 слотов. Награды в под-скриптах: cash 310,
salvage 306, heal 219, remove 408, screen 317, sample 441, infect 37.
Декоративные записи — ровно те, у которых `trigger = 0` **и** радиус 0;
тест проверяет это равенство на каждом прочитанном уровне.

## 4. Воспроизведение

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build <build> --target gm_pickup_tests -j 4
ctest --test-dir <build> -R '^pickup$' --output-on-failure
python3 regress.py --mirror --json
```

Без установки игры синтетическая половина выполняется, файловая печатает
`not installed, file checks skipped` и не падает. Зависимостей нет — только
stdlib C++.

## 5. Ограничения и следующий шаг

* Модуль **не подключён** к `GameRuntime`, `Things`, сейву, HUD/экрану конца
  уровня и к VM-опкодам 95/108/109/111/112/113/114 — это следующий срез.
* Нет радиус-скана `0x469770`, поста события `+0x44`, удаления предмета
  (op 39) и респавна/маркера `PUMARKER` (`P` §4, §7): всё это делает
  вызывающий или под-скрипт.
* Нет звука (`PLAY_SAMPLE_AT_SOURCE`, 441 вызов) и текста на экране
  (`ADD_SOURCESCREEN_TEXT`, 317): `PickupRules::last_message()` — диагностика
  модуля, а не сообщение игроку.
* Нет вируса (`SOURCEPLAYER_INFECT`, `P` §8) и лечения/порчи вещей (op 112/113
  для индексов 8..0x10b — ветки `0x45b0de`/`0x45b264`); моделируется только
  игрок.
* Лимит массы — модель и по умолчанию выключен: включённый лимит **расходится**
  с оригиналом (`E` §6).
* `DEFINE_SHIELDS` `(50, 0)` — цена/вес, а не ёмкость плит; тест проверяет
  только сами числа, ёмкость берётся из `DEFINE_PLAYER_TYPES op2 × 100`.
* `secrets_declared` — справочное поле из `SET_LEVEL_SECRETS`; ограничения на
  `secrets` нет (доказано `CF` §5).
