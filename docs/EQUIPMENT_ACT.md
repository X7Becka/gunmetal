# EQUIPMENT_ACT — активация и расход оборудования (`M6.2.4`, срез 2)

**Статус:** модель + тест на реальных `level0.sbt`/`level20.sbt`/`dm1.sbt` и на
всём корпусе 36 файлов. Файлы: `engine/src/game/equipment_act.h/.cpp`,
`engine/tests/test_equipment_act.cpp` (CTest `equipment_act`, label `quick`).
RE-канон — `docs/EQUIPMENT.md` (далее §); здесь только перенос **активации
(опкод 130) и тика расхода патронов** в C++ и явное разделение
доказанного/модели.

## 0. Итог

* Запись `DEFINE_EQUIPMENT` заполняет **вызывающий**: модуль не парсит `.sbt`
  и не знает про `EquipmentTable` (см. `game/equipment.*`).
* `activate(id)` — ровно то, что делает опкод 130: `active |= 1 << id`,
  идемпотентно, **без проверки патронов** (§4.1). Проверка
  `IF SOURCEPLAYER_AMMO_AT_LEAST` живёт в подскриптах `E3`/`E4` (§3.1/§4.2) и
  здесь не исполняется.
* `tick(ticks, ammo)` — тик `0x4463a9`: у активных записей с `E7 > 0` и
  `E8 >= 0` списывает `E7*ticks` из `ammo[E8]` (`0x81c2dc + 4*E8`), зажимает
  на 0 и один раз сообщает `no_ammo`; бит активности **не снимает** (§2.1).
* Данные: 36 файлов, **434** записи, 12 на уровень + `BALLAST` только в
  `level0`/`level20`; 13 имён. Тест перечитывает корпус и сверяет эти числа.
* Отличие модели от оригинала одно: `E6` пишется в лог **один раз** на
  переход «активировано → пул 0», а оригинал зовёт `event_post` каждый
  тик (§2.1). Это осознанная дедупликация, см. §4.1.

## 1. Что доказано (и откуда)

| факт | источник |
|---|---|
| активность — одно слово `player+0x3328` (`0x81c368`), бит `i` = запись `i` | §5 |
| опкод 130 (`0x45a816`): `active \|= / &= ~(1 << bit)`, 626 вызовов, биты 0..7 | §4.1, `tools/re/equipment.py --stats` |
| ввод бит не меняет: `FUN_00445fa0` постит `E3`/`E4`, бит ставит подскрипт | §4.1/§4.2 |
| тик `0x4463a9`: `ammo[E8] -= E7*ticks` (`0x4463eb`/`0x4463ee`), при `<= 0` → 0 и `E6` (`0x446445`) | §2.1 |
| тик не снимает `active` — включённый предмет остаётся включённым | §2.1 |
| `E7 > 0` у 6 записей из 12: `FORCE SHIELD` 10, остальные пять — 1 | §2.1/§3 |
| `E8 = -1` — пула нет; общий пул на тип патронов, не на предмет | §2.1/§3 |
| записей не больше 31, индексы 0..30 (`0x450e65`) | §2 |

Тест не пересказывает док, а читает файлы:
`gm::load_level` → `gm::decode_sbt_script` → op 175 →
`gm::EquipmentTable::from_records` для `level0`/`level20`/`dm1`, а корпусные
итоги — `gm::parse_sbt` по всем 36 `*.sbt` установки.

## 2. Данные (сверено тестом)

`level0` и `level20` — по **13** записей, `dm1` — **12**; корпус — 36 файлов,
434 записи, 13 имён, `BALLAST` ровно в двух файлах: `level0`, `level20`.

| id | имя | flags | E7 | E8 | obj | price | weight (`level0`) |
|---|---|---|---|---|---|---|---|
| 0 | `HEADLIGHTS` | 0x03 | 1 | 10 | 1 | 0 | 0 |
| 1 | `FORCE SHIELD` | 0x03 | 10 | 10 | 2 | 800000 | 650 |
| 2 | `MINE DETECTOR` | 0x03 | 0 | −1 | 3 | 40000 | 45 |
| 3 | `TARGETING` | 0x03 | 0 | −1 | 4 | 60000 | 10 |
| 4 | `SELF-DESTRUCT` | 0x00 | 0 | −1 | 0 | 0 | 0 |
| 5 | `WARP DEVICE` | 0x03 | 0 | −1 | 5 | 100000 | 300 |
| 6 | `RADAR` | 0x03 | 0 | −1 | 6 | 25000 | 30 |
| 7 | `FLIGHT PACK` | 0x08 | 0 | −1 | 0 | 0 | 0 |
| 8 | `MINE LAYER` | 0x03 | 1 | 11 | 7 | 75000 | 300 |
| 9 | `COMBAT BLOCK` | 0x89 | 1 | 12 | 8 | 0 | 0 |
| 10 | `AUTO SENTRY` | 0x89 | 1 | 13 | 9 | 0 | 0 |
| 11 | `DECOY` | 0x89 | 1 | 14 | 10 | 0 | 0 |
| 12 | `BALLAST` | 0x0c | 0 | 0 | 0 | 0 | **850** / **2000** |

**Поправка к `docs/EQUIPMENT.md` §3:** масса `BALLAST` не одна и та же:
в `level0.sbt` — **850**, в `level20.sbt` — **2000** (все остальные поля
совпадают). Док §3 указывает только 850; тест печатает оба числа и
фиксирует их.

Активация не смотрит на `flags`: `op 130` — безусловный OR. Поэтому
`BALLAST` (0x0c) технически активируем, хотя скрипты дают ему только бит
«куплено» (`OR_SOURCEPLAYER_EQUIPMENT_BITS 0x1000`, §3) и в циклическом
переборе он не участвует (`flags & 0x04` — забота `equipment_state.*`).

## 3. API

```cpp
namespace gm::equip {

struct EquipmentRecord {
    int id = -1;                 // индекс записи; активный бит = 1 << id
    std::string name;
    std::uint32_t flags = 0;     // E0
    int ammo_type = -1;          // E8, -1 = пула нет
    int ammo_per_tick = 0;       // E7
    int price = 0;               // E9
    int weight = 0;              // E10
    int object_id = 0;           // E11
};

struct EquipmentEvent {
    enum Kind { activated, deactivated, no_ammo };
    int id = -1;
    Kind kind = activated;
};

class EquipmentAct {
    static constexpr int kMaxRecords = 31;   // §2, 0x450e65
    static constexpr int kAmmoTypes = 16;    // 0x81c2dc + 4*E8
    static constexpr int kNoAmmoType = -1;   // §2/§3

    bool set_records(std::vector<EquipmentRecord> records,
                     std::string* error = nullptr);
    int  count() const;
    const EquipmentRecord* record(int id) const;

    bool activate(int id);          // op 130, on  — идемпотентно
    bool deactivate(int id);        // op 130, off — идемпотентно
    bool is_active(int id) const;
    std::uint32_t active_mask() const;   // player+0x3328
    bool is_exhausted(int id) const;     // модель: E6 уже был в этой активации

    void tick(int ticks, int* ammo);     // ammo — ровно kAmmoTypes int
    const std::vector<EquipmentEvent>& events() const;
    void clear_events();
    const std::string& last_error() const;   // текст последнего отказа
};
}
```

Семантика:

* `id` — индекс записи, а не `object_id` (горячей клавиши). Бит — `1 << id`;
  в поставе скрипты трогают только биты 0..7 (626 вызовов), но сам опкод
  принимает любой бит 32-битной маски, поэтому модель не запрещает 8..11.
* `activate` уже активного — `true`, без изменения маски и без второго
  события (OR идемпотентен). `deactivate` уже выключенного — так же.
* `activate` **не проверяет патроны** и не отказывает при нулевом пуле:
  в оригинале это делает подскрипт (§3.1), а не опкод.
* `tick` берёт `ammo` как массив из 16 `int`; вне диапазона `ammo_type`
  записи не бывает (отсекается `set_records`), а запись без пула (`E8 = -1`)
  и с `E7 = 0` пропускается.
* `events()` — журнал переходов по порядку операций; `activated`/`deactivated`
  соответствуют E3/E4 записи, `no_ammo` — E6 (§2).

## 4. Модель, а не RE

1. **`no_ammo` один раз на активацию.** В оригинале тик при каждом проходе
   пишет `E6`, если пул `<= 0` (§2.1, вызов `0x446445` внутри цикла). Журнал
   событий с таким повтором бесполезен, поэтому модель запоминает защёлку:
   событие пишется при первом переходе в 0, повторные тики его не дублируют.
   `activate` неактивной записи защёлку сбрасывает (новая активация снова
   может сообщить один раз), а `activate` уже активной — нет (это no-op).
2. **`EquipmentEvent`.** Оригинал постит числовые id (`-30043` и т.п.) через
   `event_post`; здесь — структурный журнал, чтобы вызывающий сам решил,
   какой id постить (их отдаёт `gm::EquipmentDef`). Числовые id в модуль не
   заводятся: он не парсит данные уровня.
3. **`tick(0, …)` — no-op.** Оригинал посчитал бы `E7*0 = 0` и всё равно
   запостил `E6` для уже пустого пула; модель этого не делает.
4. **Отказы `ticks < 0` и `ammo == nullptr`.** В оригинале таких значений нет
   (тик всегда положительный, пул — глобальный массив); это защита API.
5. **`set_records` не проверяет `object_id`/`flags`.** Их проверяет срез 1
   (`equipment_state.*`: горячая клавиша адресует одну запись, `BALLAST`
   отсекается по `flags & 0x04`). Здесь дублируется только то, что нужно
   маске и пулам: `id` (0..30, без дублей), `ammo_per_tick >= 0`,
   `ammo_type` в −1..15.

## 5. Отказы (все с текстом)

| вход | текст (сокращённо) |
|---|---|
| пустой набор | `set_records: empty table … DEFINE_EQUIPMENT (docs/EQUIPMENT.md §2)` |
| > 31 записи | `… 32 records, the store takes at most 31 (0x450e65 …)` |
| `id` вне 0..30 | `set_records[0] 'HEADLIGHTS' (id 31): id outside 0..30 …` |
| повтор `id` | `set_records[1] 'FORCE SHIELD' (id 0): duplicate id 0 …` |
| `ammo_per_tick < 0` | `… ammo_per_tick -1, expected >= 0 (E7, 0x4463c0 …)` |
| `ammo_type` вне −1..15 | `… ammo_type 16 outside -1..15 (E8, 0x81c2dc + 4*E8 …)` |
| `activate`/`deactivate` неизвестного id | `activate: id 13 is not in the records (13 adopted …)` |
| `ticks < 0` | `tick: got -1 ticks, expected >= 0` |
| `ammo == nullptr` | `tick: null ammo table, expected 16 ints …` |

Неудачный `set_records` оставляет объект пустым (`count() == 0`,
`has_records() == false`, маска 0); успешный — очищает `last_error()`.

## 6. Числа теста

```sh
cmake --build build/sandbox --target gm_equipment_act_tests -j 4
ctest --test-dir build/sandbox -R '^equipment_act$' --output-on-failure
python3 regress.py --mirror --json
```

Вывод (сокращённо; таблица всех 13 записей печатается целиком):

```text
equipment_act: corpus files=36 records=434 names=13 ballast_in= level0 level20
equipment_act: records level0=13 level20=13 dm1=12
equipment_act: BALLAST weight level0=850 level20=2000
equipment_act: id= 0 HEADLIGHTS    mask=0x00000001 active=1 repeat=noop events=2
...
equipment_act: id=11 DECOY         mask=0x00000800 active=1 repeat=noop events=2
equipment_act: all 12 active mask=0x00000fff events=12 BALLAST(id 12) not activated (flags 0x0c)
equipment_act: HEADLIGHTS E7=1 ammo10=100 tick(10) -> 90 events=1
equipment_act: HEADLIGHTS deactivate + tick(10) -> ammo10=90
equipment_act: HEADLIGHTS ammo10=5 tick(10) -> 0 no_ammo=1 active=1
equipment_act: re-activate at 0 -> no event, tick(1) no_ammo=0
equipment_act: off/on at 0 + tick(1) -> no_ammo=1 (once per activation)
equipment_act: FORCE SHIELD E7=10 ammo10=95 tick(3) -> 65
equipment_act: MINE LAYER+COMBAT BLOCK tick(3) -> ammo11=7 ammo12=17 ammo10=7
equipment_act: tick(7)+tick(10) -> ammo11=0 ammo12=0 no_ammo8=1 no_ammo9=1
equipment_act: both off + tick(4) -> ammo11=5 ammo12=6
equipment_act: determinism mask=0x00000503 events=11 ammo10=0 ammo11=0 ammo13=0
equipment_act: ok
```

Ключевые проверки:

* маска после каждой одиночной активации — ровно `1 << id` (id 0..11), повтор —
  `no-op`, `events` не растёт; после `deactivate` маска 0;
* `E7 * ticks`: `HEADLIGHTS` 100 → 90 за 10 тиков; `FORCE SHIELD` 95 → 65 за 3
  (E7 = 10);
* граница: пул 5 и 10 тиков → 0, ровно **одно** `no_ammo`, `active` остался 1,
  повторные тики события не добавляют; выключение/включение при 0 даёт ещё
  ровно одно;
* два активных с разными типами (`MINE LAYER` тип 11, `COMBAT BLOCK` тип 12)
  тратят каждый свой пул, чужие пулы (в т.ч. тип 10) не меняются;
* `deactivate` останавливает расход (пулы не меняются);
* два одинаковых прогона: та же маска, те же пулы, тот же журнал событий;
* негативы из §5 — все с текстом.

`regress.py --mirror --json`: `{"passed": 115, "total": 115, "failed": []}`.
CTest `equipment_act` — Passed, 2.85 с (включая чтение 36 `.sbt`).

## 7. Ограничения и следующий срез

Не сделано осознанно (чтобы не пересекаться с `equipment.*`,
`equipment_state.*`, `runtime.*`, `vm_exec.*`):

* нет подключения к `GameRuntime`, `VmExecutor`, вводу, HUD и сейву:
  `VmPlayer::equipment_active`/`equipment_bits` остаются авторитетными для
  скриптов, `equipment_input`/`equipment_tick` — отдельными функциями;
* подскрипты `E3`/`E4` не исполняются: проверка патронов, тексты
  `NO POWER FOR …`/`AMMUNITION DEPLETED`, `HEADLIGHT`/`FLY`/`DETECTOR`,
  спрайты и `DAMAGE_MODIFIER` — это опкоды уровня (§3.1/§6.1), а не
  состояние; модуль лишь отдаёт `activated`/`deactivated`/`no_ammo`;
* магазин, покупка/продажа, смена шасси и `sell_all` не моделируются:
  `owned`/`index`/`cash`/масса — §4.1/§5 и `docs/ECONOMY_STATE.md`;
* нет `E10`/`E9` в логике: цена и масса только переносятся в запись;
* `E6`-дедупликация — отклонение от «каждый тик», описанное в §4.1; если
  понадобится точная эмуляция `event_post`, защёлку надо отключать флагом.

Следующий срез: связать `EquipmentAct` с `EquipmentState`/`equipment_input`
и `GameRuntime` (один экземпляр на игрока, `events()` → `event_post`,
`active_mask()` → `player+0x3328`, `tick` после `equipment_tick`), не меняя
поведение VM.
