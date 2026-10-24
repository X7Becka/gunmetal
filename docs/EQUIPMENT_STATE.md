# EQUIPMENT_STATE — состояние оборудования в рантайме (`M6.2.4`, срез 2)

**Статус:** модель + тест на реальных `level0.sbt`/`dm1.sbt`. Файлы:
`engine/src/game/equipment_state.h/.cpp`, `engine/tests/test_equipment_state.cpp`
(CTest `equipment_state`, label `quick`). RE-канон — `docs/EQUIPMENT.md`
(далее §); этот документ описывает только перенос состояния в C++ и явно
отделяет доказанное от модели.

## 0. Итог

* Данные читаются готовым `game/equipment.*` (`EquipmentDef` E0..E11, предел
  31 записи, `EquipmentTable::from_records`); срез 2 добавляет **состояние**:
  слоты, активность, общий пул патронов, кулдаун, выдачу id событий.
* `level0` — **13** записей (`BALLAST`), `dm1` — **12**; в обоих **12**
  доступных слотов (`BALLAST` — `flags & 0x04`, вне цикла перебора, §3).
* «Активация» в модели — это **решение скрипта уровня**, а не физический
  эффект: опкод 130 ставит бит `active` (§4.1), а свет/щит/полёт/детектор —
  отдельные опкоды подскрипта (§3.1, §6.1). Модуль не рисует и не двигает
  ничего, он только считает состояние и отдаёт id событий.
* Кулдауна в оригинале **нет** (ни в записи, ни в блоке игрока) — это
  добавка модели, см. §4.

## 1. Что доказано (и откуда)

| факт | источник |
|---|---|
| запись = 12 dword + имя; E0..E11 → `+0x00..+0x28`, `+0x3c` | §2, стор `0x450e60` |
| максимум **31** запись, индексы 0..30 | §2, `0x450e65` (`cmp eax,0x1f; jl`) |
| `E1` spawn, `E2` selection, `E3` on, `E4` off, `E6` no-ammo | §2 (читатели `0x45427f`, `0x45a542`, `0x446228`, `0x44621d`, `0x446445`) |
| `E7` — расход патронов за тик, `E8` — тип патронов (−1 = нет) | §2.1, `0x4463c0`/`0x4463cb` |
| патроны — **общий пул на тип** (`0x81c2dc + 4*E8`), не на предмет | §2.1, `0x4463ee` |
| тик, дошедший до нуля, постит `E6`, но **не снимает** `active` | §2.1 |
| расходники (idx 8..11) бит активности не ставят вообще | §3.1 |
| `flags 0x04` — «вне цикла перебора» (у `BALLAST`), `0x08` — скрыт | §3, `0x445ff1`/`0x4460f5`, `0x47b6f5` |
| горячие клавиши — `object_id` 1..10; 0 = клавиши нет | §4.1, `0x44623d..0x44638d` |
| `object_id`, `ammo_type` в поставе не повторяются/в диапазоне | `tools/re/equipment.py --check` (36/36) |
| `E2` во всех 36 уровнях = 0 (путь поддержан, но не используется) | §4.1 |

Проверка данных в тесте — не пересказ дока, а чтение реальных файлов:
`gm::load_level` → `gm::decode_sbt_script` → `DEFINE_EQUIPMENT` (op 175) →
`gm::EquipmentTable::from_records`, и сверка **всех 13** записей `level0` с
таблицей §3 по именам и пяти id событий.

## 2. Модель

`namespace gm::equip` (не `gm`, потому что `gm::EquipmentState` — уже
существующая POD-структура `+0x3320/+0x3324/+0x3328` в `game/equipment.h`;
её и `equipment_input`/`equipment_tick` срез 2 не дублирует и не меняет).

```
EquipSlot { id; active; ammo; cooldown }        // id = индекс записи
EquipmentState::load(defs)                       // из DEFINE_EQUIPMENT
  activate(slot)   -> бит active = 1 (как op 130), событие E3
  deactivate(slot) -> бит active = 0, событие E4
  consume(slot)    -> общий пул ammo_type -= 1, бит НЕ трогает
  tick()           -> только cooldown -= 1 (1/91 с)
  event_for(slot, kind) -> E1/E2/E3/E4/E6 как u32
```

Порядок проверок `activate` (каждая — по доку):

1. слот в `0..slots()-1`, иначе `false` + текст;
2. уже включён → `false` + текст (повторный op 130 идемпотентен, но модель
   считает это ошибкой вызывающего);
3. `flags & 0x04` → `false` (вне цикла, §3; так отсекается `BALLAST`);
4. `ammo_type >= 0` и пул `<= 0` → `false` + `last_event = E6` (§2.1);
5. `cooldown > 0` → `false` + текст (модель, §4);
6. иначе `active = true`, `last_event = E3` — это тот запрос, на который
   отвечает подскрипт (§4.2).

`ammo` в слоте — **зеркало** общего пула своего `ammo_type`; оно
пересчитывается при `set_ammo*`, `consume` и `sync_ammo`. `HEADLIGHTS` и
`FORCE SHIELD` (оба тип 10) видят одно число, как в оригинале.

`tick()` не тратит патроны: расход `E7*ticks` уже реализован в
`gm::equipment_tick` (§2.1), и дублировать его здесь нельзя. После внешнего
тика пул принимается через `sync_ammo`.

## 3. API

```cpp
namespace gm::equip {

enum class Event { Spawn = 1, Activate = 2, On = 3, Off = 4, NoAmmo = 6 };
struct EquipSlot { int id; bool active; int ammo; int cooldown; };

class EquipmentState {
    static constexpr int kMaxSlots = 31;          // §2, 0x450e65
    static constexpr int kNoAmmoType = -1;        // §2
    static constexpr int kAmmoTypes = 16;         // 0x81c2dc + 4*E8
    static constexpr std::int32_t kFlagNotInCycle = 0x04;  // §3
    static constexpr std::int32_t kFlagHidden     = 0x08;  // §3

    bool load(const std::vector<EquipmentDef>& defs, std::string* error = nullptr);
    int  slots() const;      // 0x815420
    int  usable() const;     // !(flags & 0x04)
    bool has(int slot) const;
    const EquipSlot& at(int slot) const;        // вне диапазона: id = -1
    const EquipmentDef* record(int slot) const;

    bool activate(int slot);
    void deactivate(int slot);
    bool consume(int slot);
    void tick();

    bool set_cooldown(int slot, int ticks);
    bool set_ammo(int slot, int value);
    bool set_ammo_for_type(int type, int value);
    int  ammo_for_type(int type) const;
    void sync_ammo(const AmmoPool& ammo);

    std::uint32_t event_for(int slot, Event kind) const;
    std::uint32_t last_event() const;
    int  last_event_slot() const;
    void clear_event();
    const std::string& last_error() const;
};
}
```

`Event` — индекс dword'а в записи (`fields[1/2/3/4/6]`), поэтому `event_for`
отдаёт число **побайтово как в файле**: отрицательные id (`-30043`) приходят
как `0xFFFF8AA5` — тот же u32, который постит `event_post`.

Отказы `load` (все с текстом, состояние остаётся пустым): пустой набор;
> 31 записи; `object_id` вне 0..10; повтор непустого `object_id` (клавиша
должна адресовать одну запись, §4.1; три записи с `object_id = 0` в `level0`
— не дубли); `ammo_type` вне −1..15.

## 4. Что здесь модель, а не RE

1. **`cooldown`.** В 1.36 ни запись, ни блок игрока счётчика кулдауна не
   имеют (§2, §5) — ограничение темпа у оригинала получается из фронта
   нажатий (бит действия на кадр) и расхода патронов. Поле введено как
   управляемый вызывающим лимит в тиках (0 = свободно): `activate` при
   `cooldown > 0` отказывает, `tick()` уменьшает. Начальное значение — 0,
   данными уровня не заполняется.
2. **Событие при пустом пуле.** Оригинальный подскрипт в этой ветке печатает
   текст (`NO POWER FOR HEADLIGHTS`, `AMMUNITION DEPLETED`, §3.1) и/или
   `GENERATE`, а `E6` постит **тик** при переходе пула в 0 (§2.1, §9.4).
   Модель использует `E6` записи как «нет патронов» — это решение API, а не
   наблюдённое поведение.
3. **`consume` не деактивирует.** Проверено по доку: тик не снимает бит
   (§2.1), расходники его и не ставят (§3.1). Поэтому «при 0 — deactivate»,
   который напрашивался, **не** реализован; тест это фиксирует
   (`HEADLIGHTS` после `consume` до 0 остаётся `active = 1`).
4. **`activate` для записей с `object_id = 0`** (SELF-DESTRUCT, FLIGHT PACK)
   разрешена: у них нет горячей клавиши, но общий toggle их включает (§4.1).
   `BALLAST` отсекается только по `flags & 0x04`.

## 5. Числа теста

```sh
cmake --build build/sandbox --target gm_equipment_state_tests -j 4
ctest --test-dir build/sandbox -R equipment_state -V
python3 regress.py --mirror --json
```

Вывод теста (сокращённо; полный — 13 строк таблицы `level0`):

```text
equipment_state: level0=13 dm1=12 records
equipment_state: level0 records=13 usable=12
equipment_state: dm1 records=12 usable=12 last=DECOY
equipment_state: activate HEADLIGHTS ammo10=25 -> active=1 event=-30043
equipment_state: deactivate HEADLIGHTS -> active=0 event=-30044
equipment_state: activate FORCE SHIELD ammo10=0 -> false event=-30046 err=... no ammo ...
equipment_state: tick -> MINE DETECTOR cooldown=2/1/0
equipment_state: activate BALLAST flags=0x0c -> false err=... flags & 0x04 ...
equipment_state: MINE LAYER consume 2x -> ammo=0 active=0
equipment_state: HEADLIGHTS consume to 0 -> ammo=0 active=1 event=-30044
equipment_state: slot -1/13 -> at.id=-1 err=... outside 0..12
equipment_state: load empty -> false err=... empty table ...
equipment_state: load 32 records -> false err=... at most 31 ...
equipment_state: duplicate object_id 5 -> false err=... duplicate object_id 5 ...
equipment_state: object_id 11 / ammo_type 16 -> false err=...
equipment_state: ok
```

Сверено с §3 построчно для `level0`: 13/13 имён, 13×5 id событий, `flags`,
`object_id`, `E7`, `E8` (например `HEADLIGHTS` 0/0/−30043/−30044/−30044,
`WARP DEVICE` E4 = 0, `RADAR` E6 = 0, `MINE LAYER` E3 = 1000018).

## 6. Ограничения и следующий срез

Не сделано (осознанно, чтобы не пересекаться с `equipment.*`/`vm_exec.*`):

* нет подключения к `GameRuntime`/`VmExecutor`: `equipment_input` (next/prev/
  toggle/hotkey) и `equipment_tick` остаются отдельными функциями, а
  `VmPlayer::equipment_bits/active/index` — авторитетными для скриптов;
* нет `owned`/`index`/`price`/`weight`/массы: покупка, продажа, HUD, сейв —
  по-прежнему §5/§8 `docs/EQUIPMENT.md`;
* нет звуков, анимаций, спрайтов и эффектов (свет, щит, полёт, детектор,
  таргетинг): это опкоды подскриптов (`§3.1`, `§6.1`), а не состояние;
* `E5` (`+0x14`) не моделируется — читателей нет, в данных 0 (§9.1);
* счётчик `ticks` расхода `E7*ticks` (§9.3) остаётся за `equipment_tick`.

Следующий срез: связать `gm::equip::EquipmentState` с `GameRuntime` (один
экземпляр на игрока, `sync_ammo` после `equipment_tick`, `last_event` →
`event_post`) и с `equipment_input`, не меняя поведение VM.
