# NEXT — очередь автономной работы

Обновляется в конце каждого хода. Если пользователь не написал `STOP`/`пауза`,
агент берёт верхний незаблокированный пункт и работает дальше без разрешения.

## Сейчас

1. **Снаряды / скорость.** `thing_variants` PLASMA 36/37/38 загружены;
   `projectile_speed = THING_TYPES+0x10 × 91` (ед/тик доказаны `0x4752d0`),
   12000 остаётся fallback’ом. Impact/expire-ветки GRENADE/ROCKET/RGM/MRL/
   NUKE/EMP/DISINT загружены и применяются через `apply_weapon_blast`;
   `apply_weapon_inline` применяет EMP-кольца, `fire(..., charge)` считает
   `speed = base + charge*operand[9]`. **Заряд разобран и портирован:**
   `0x4453d0` — `player+0x2fa0` (защёлкнутый заряд) и `+0x2fa8`
   (`accumulated += dt*weapon[+0x00]`), `0x46f0f0` читает первое;
   `gm::update_charge`/`ChargeState` повторяют режимы 0/1/2..6 (граница
   ровно 1.0 не стреляет, авто-выстрел только у 3/5), реальные зарядные
   оружия — GRENADE/VIRAL AGENT (mode 2, rate 0.015/тик). Осталось:
   ввод игрока (курок/слот) в игровом цикле и передача `state.charge` в
   `fire(...)`.
2. **AI wiring.** 240-кадровая история цели с лагом `target_history_offset`
   готова; **виталы вещи подключены**: `Ai::sync_vitals()` зеркалит HP
   (+0x1d4), щиты (+0x1d8) и счётчик попаданий (+0x1c8), мёртвая вещь убивает
   агента (урон оружия виден на следующем тике, `Things` AI не мутирует).
   `history_lag` даёт `aim_point`, скан до 8 игроков — отдельное кольцо на
   каждого, fallback-цели — живые вещи (`target_thing`), статусы
   `burn/status` зеркалятся. Осталось: навигация, реальное оружие/урон
   игроку и использование `aim_point` в игровом слое.
3. **Event effects.** Реестр текста `0x619860` **разобран и перенесён**:
   `gm::TextRegistry` — 10 слотов, дедуп по (текст, kind, font), срок жизни
   `now+0x111`, обмен «последний → освободившийся» при истечении, отбор по
   каналу (`-1` или запрошенный) с сортировкой по `updated`; текст до 0x31
   символа. Побочные эффекты switch (`0x44f960` — `GENERATE`-событие в точке
   переключателя + 3D-звук `0x2b11`) и сетевой блоб (`u32` длины плюс
   записи) — разобраны и описаны в `docs/EVENTS.md` §2.1–§2.3; в C++ —
   `gm::EventQueue::parse_network_blob()`/`accept_network_blob()`, runtime-
   таблица `0x5a4af0` — `gm::EventDispatcher` (`serialize`/`deserialize`,
   `install_switches`), THING-события — `post_thing_event`. Осталось:
   подключить таблицу/пакет к игровому циклу.
4. **Player integrator.** Интегратор оригинала **доказан исполнением его
   байтов**: `tools/re/player_integrator.py` выполняет цикл
   `0x444476..0x4444d5` — `v ← v·k + Δv` (`k = 0x81bfb8` = retention, 0.9 у
   LIGHT по сейву), смещение = сумма обновлённых скоростей,
   `vz ← 0.85·vz + 0.15·pitch`; расхождение с замкнутой формой — 5·10⁻¹⁶
   (одна единица в последнем разряде: x87 держит 80-битные промежуточные).
   Порт в C++ (`integrate_velocity`/`integrate_batch`/`integrate_vertical`/
   `input_delta`/`weight_limited_speed`/`nominal_speed`/`chassis_from_config`)
   и его голдены — вывод эмулятора; `docs/PLAYER_GHIDRA.md` §4.8–§4.9,
   `docs/PLAYER_CONTROLLER.md` §3.11. Вход (`Δv`) — знаковая разность
   счётчиков действий (`0x81c3ac−0x81c3b0`, `0x81c388−0x81c38c`; блок
   копируется из `0x5a49f0` в `0x443690`). `op4` — **номинал** (ед/тик), не
   ускорение и не предел: `ControllerConfig::max_speed` = конверсия
   номинала (`1.70·91`), `retention` = k. Осталось: числовые значения `n` и
   `param_2` в живом кадре (нужен отладчик оригинала) и подвеска/коллизии
   оригинала — до них `step()` остаётся синтетикой.
5. **Полный регресс.** C++ quick 39/43 (4 падения, все внешние), `regress.py`
   121/121 после восстановления `/tmp/opencode/gundx.exe` (tmpfs стирается при
   `wsl --shutdown`, `savegame.py` ждёт exe именно там). Внешние падения:
   `core`/`sbtvm`/`vm` — `level1.sbt` и `level2.sbt` в установке **поменяны
   местами** относительно голденов (fnv `2aa7…`/`7e6d…` ровно наоборот;
   `level1` отдаёт только DM-спавны, `level2` — кампанийные), `camera_rig`
   ждёт кампанийный спавн `level1`, которого в файле нет. Правки кода тут ни
   при чём — чинится только подменой файлов в игре или пересъёмкой голденов;
   игру не трогаем.

6. **Ghidra-карты структур (M2.3).** Сделано: `M2.2.1` ✅ (`docs/INIT.md`,
   `tools/re/init_flow.py`), `M2.3.1` 🟡 (`docs/PLAYER_FIELDS.md`),
   `M2.3.3` 🟡 (`docs/THING_FIELDS.md`; заодно исправлен `SOE +0x010`:
   `speed`, не `off_z` — C++/Python/test/regress согласованы), `M2.3.5` 🟡
   (`docs/WEAPON_FIELDS.md`; `charge_rate` в памяти = файл × 0.91, уже в
   `Weapons::load`). В работе: `M2.3.2` (мир/сектор/полигон), `M2.3.4`
   (дверь/переключатель/лифт), `M2.3.7` (звук). Не начато: `M2.3.6`
   (скриптовая таблица сущностей), `M2.2.3` (загрузка/выгрузка уровня).
7. **`FORMATS.md` синхронизирован.** ✅ Внесены все уточнения из карт:
   `+0x18` игрока — углы (не позиция), `+0x2ff8` — точки навесных частей,
   шасси model index `+0x154` (не `+0x28`), `charge_rate × 0.91`,
   `muzzle = -1`, `THING_TYPES+0x010 = speed`, §12.4 — биты флагов
   сущностей 0..7 (игроки), 0x100 (не-игрок), 0x200 (one-shot).
8. **Ввод игрока → выстрел.** ✅ `game/trigger.{h,cpp}` (`PlayerWeaponInput`,
   `update_player_weapon`) + `GameRuntime::update_player_weapon`; выстрел идёт
   через `spawn_player_shot`. Доказано байтами: курок — действие 5 (`0x4bc558`
   → `0x4bc8ec` `"Fire"`), маска `0x61f1e8[player]`, `FUN_004650a0` пишет
   удержание в `player+0x3340` и фронт в `+0x3344`; кулдаун `+0x3318`
   (`0x4453f8..0x445410`, гейт `0x445aa2`); подавление state-события
   `0x445a4a` (`TEST [player+0x3340],0x20`); полуавтомат `0x445d85`
   (фронт), автомат `0x445d64` (удержание); слот — биты `0x2000/0x4000` в
   `+0x3344` (`0x445547`/`0x44564a`), `FUN_00445300` пишет `+0x3294` и
   обнуляет `+0x2fa8`; заряд `0x445af6..0x445c63`, граница `1.0`
   `0x445cf1`; патроны `0x445dfb/0x445e98`; muzzle `0x4458c4`. CTest
   `player_trigger`. Гипотезы: сканирование соседних слотов, кулдаун по слотам
   (в оригинале один `+0x3318`), mode 4/5 как непрерывный огонь. Детали —
   `docs/WEAPONS_CPP.md` §3.6.
9. **Урон через события, а не через `Weapons`.** ✅ Реестр
   `gm::EventBlastRegistry` (`events.{h,cpp}`) + `gm::apply_event_blast`;
   `ProjectileTickRecord` несёт `collide/expire/destroy_event`;
   `GameRuntime::apply_projectile_blasts(registry, …)` берёт id снаряда и
   применяет blast к игрокам и вещам. Цепочка доказана: collide `0x4752d0`
   (`THING_TYPES+0x1c4`), expire `0x4703a0` (`+0x1ec`), `event_post 0x4795b0`,
   диспетчер `0x4799b0`/`0x479960`, `generate_runner 0x45fa00` → VM → op 75
   `0x45775b` → `0x477280` (**единственный** blast-урон в бинаре); игрок
   `0x453b50` (броня `+0x3244/+0x3248`, плиты `+0x3250..5c`, смерть
   `0x4536e0`), вещь `0x470500` (`+0x360/+0x398/+0x39c`). Weapon-мост помечен
   legacy. Осталось: `param`/координаты на цель при постинге, связь
   proximity-события `+0x21c` с blast (не доказана). Детали —
   `docs/PROJECTILE_CPP.md` §7.

10. **`SECTOR_STUFF` перенесён полностью.** ✅ `.sbt`-запись 8 —
    `SbtSectorSlot`/`parse_sector_stuff`/`encode_sector_stuff`; блок 8 сейвов
    (`res/save.{h,cpp}`) типизирован (`Save::sector_stuff`, 256 слотов),
    `rebuild_save(verify)` сверяет поле-в-поле; VM пишет слоты
    (`VmState::sectors[256]`, `h_sector` для опкодов 8/11/12/13/14, засев
    `vm_load_sector_stuff`). Доказано: таблица диспетчера `0x45c500`
    (индекс `opcode-1`), `fread/fwrite(0x800b60, 0x5800)`
    (`0x46b7b0`/`0x46b7f0`), очистка `0x46b1b0`; `+0x40/+0x44` — **два
    dword'а** (не два u16), `+0x44` читается как знаковый; потребители
    `0x4713ab` (движение вещей), `0x4450f6` (урон → `0x453b50`),
    `0x46c5f3` (ambient), `0x44503f`/`0x445095` (`event_post`). Тесты
    `sector_stuff`, `save_sector_stuff`, `vm`. Осталось: `SET_SOURCE/HANDLE
    SECTOR_*` (27–31, 40–45) — нужен маппинг «сущность → сектор»;
    `+0x18`/`+0x54` без читателей; смысл `damage_a` vs `damage_b` не
    разделён. Карта — `docs/RECORDS.md` §2.

## Стоп-условия

- Пользователь написал `STOP`/`пауза`.
- Все пункты выше закрыты или помечены `⛔` с причиной.
- Нужно необратимое действие/секрет, которого нет.

## Последний ход

- **Entity rows + SLEEP смержены** (`feat/entity-flags`). Доказано по байтам
  `0x45fa83..0x45fb1c`: one-shot `0x200` сбрасывает `0x1ff` **до** `CALL
  0x457390` (маска `0xfffffe00` сохраняет бит 9: `0x3ff → 0x200`), только у
  запускаемой строки; `ENABLE` `0x457c80` снова даёт `0x3ff`, `DISABLE`
  бит 9 не трогает. Дубли id (249 строк) идут циклом без `break`, каждая со
  своим тестом источника. C++: `VmState::entities` — вектор всех строк
  (табличный порядок) + `entity(id)`/`entity_count(id)`, последовательный
  запуск дублей через `next_generate_row`/`row_cursor`/`leave_frame`.
  SLEEP — девять массивов `0x400` (`0x5e4278` тики, `0x5e3df0` offset,
  параметры `0x616258/0x5e5e58/0x5e2dd8/0x5e39d8/0x5e35d8/0x5e31d8`,
  хэндл `0x5b29c8`): `delay = операнд − carry`, `delay>0` — слот и возврат
  из `0x457390`, тикер `0x457230`, cid 198 освобождает слоты источника,
  предел 256. В поставке `flag = 0`, но 7954 команды `SLEEP`.
  Golden `level20` main 36 → 34 (строка с `flags = 0` в игре не
  запускается). `vm` 15 / `vm_full` 28 падений — ровно прежние level1/level2.

- **Автоматы движения вещей смержены** (`feat/thing-motion`):
  `engine/src/world/thing_motion.{h,cpp}` (772+225 строк) — режимы
  `0..18` из switch `0x470c70`: 0/1 (статика/вращение), 2 (`0x474260`,
  3 зонда по 300, `speed·dt` при `d_fwd > speed·dt+radius`), 3/4
  (`0x4746b0`), 5 (`0x474ea0`, турель — только yaw), 6 (`0x4752d0`,
  `vz += 0.2·g·dt`, отскок, hits→expire), 13 (`0x474a10`), 14/15/17
  (`0x473240`), 16 (`0x476070`, патруль по полосам `p0/p1` + кулдаун
  `p2`); 7/8 → `projectile`, 9–12 → `ai_move`, 18 — stub.
  Писатели `+0x208/+0x210` — только `0x472120`, толкают вдоль `right`
  (`object+0x18`), не вперёд. CTest `thing_motion` (91 тик на 121/141
  экземплярах `level0`/`dm1`, побитовый детерминизм). Поправки в
  `docs/THING_FIELDS.md` §4a и `docs/AI_CPP.md` §10–11.
  `things_full` падает только на внешней паре level1/level2.

- **Карта материалов (M2.3.2, остаток) смержена** (`b6d4fc5`):
  `docs/WORLD_MATERIALS.md` + `tools/re/materials.py` (`--map`/`--verify`
  371 проверка/`--self-test`). Таблица `0x5670e0` разобрана: `+0x10` —
  light-LUT (`0x4409c5`), `+0x34` — имя (`0x42eaf5`), `+0x2c` —
  `0/64/64/100/127/−128`; overlay `poly+0x78` (луч `0x437d05`, печёт
  `0x43e69d`), scroll `+0x5c/+0x60` (`0x434c11`), anim `+0x5e/+0x62/+0x66`
  (`0x433e00`), brightness `+0x70` (`0x43c51f`), флаги объекта `+0x70`
  (`0x10010` cull, `0x20000` форс-рендер, `0x40000/0x80000`, `0x100000`),
  `+0x80` — экземпляр вещи (пул `0x71cd40`), `+0x74` — знаковая поправка
  яркости. Ряд полей (`+0x04..0x0c`, `+0x14`, `+0x20..0x30`) в `gundx.exe`
  **не читается** (xref + байтовый скан) — вероятный потребитель `gun.exe`.
- **Найден дефект трансформа `.sbw`** (проверено данными): заголовок объекта —
  12 × `f64` (origin + базис, в поставке базис единичный), а код
  (`gm/sbw.py::_points`, `render/collide.cpp::decode_transform`,
  `render/render.cpp::header_points`) читает его как `(u32 pad, f32)` и
  получает `diag(1.875)`. `1.875` — это float-биты старшего dword от double
  `1.0`. Рантайм читает `FLD double` (`0x437af8`), загрузчик копирует 96 Б без
  конверсии (`0x42ae86`). Идут 4 агента: трансформ, движение вещей,
  one-shot/SLEEP-таблица, геометрия дверей.

- **Незакоммиченное разобрано.** В master-дереве висели правки `docs/FORMATS.md`
  (это был отложенный пункт 7) — проверены по картам и закоммичены (`c281e1c`);
  `sector-stuff` worktree содержал недокоммиченный парсер `SECTOR_STUFF` —
  собран, прогнан (тест `sector_stuff`: 36/36 уровней без записи 8, блок 8
  сейвов 13/13 слотов против оракула) и смержен (`7d65c1d`). Старые
  worktree/ветки (`qa-f4-switch-source`, `feat/entity-fields`,
  `feat/event-blast`, `feat/player-fire`, `feat/sector-stuff`,
  detached `runtime-hard`) удалены — все были влиты. `regress.py --full`
  после слияния — 121/121.
- **Запущены 4 фоновых агента** (каждый в своём worktree в `/var/tmp`, без push):
  `feat/player-input` — ввод → `update_charge` → `fire` → `spawn_player_shot`;
  `feat/sector-save` — блок 8 в `res/save.cpp` + `SET_SECTOR_*` в VM;
  `feat/blast-damage` — урон от `collide`/`expire`-событий игрокам и вещам;
  `feat/materials` — потребители таблицы материалов и overlay/scroll/anim
  (`docs/WORLD_MATERIALS.md`, `tools/re/materials.py`).

- **Пункт 2 (AI wiring) — витали вещи подключены.** `Ai::sync_vitals()`
  зеркалит HP/щиты/hits вещи в агента и убивает агента вместе с вещью;
  `tick()` зовёт её до состоянийной машины, поэтому урон оружия виден на
  следующем тике. CTest `thing_ai` — 1/1. Коммит `dc06665`.
- **Пункт 3 (event effects) закрыт настолько, насколько позволяет статика.**
  Реестр сообщений `0x619860` разобран по коду (`0x463ab0` post,
  `0x463e00` expire, `0x463bc0`/`0x463ef0` draw) и перенесён в C++
  (`gm::TextRegistry`, `gm::post_event`); побочные эффекты типа 2
  (`0x44f960`) и сетевой блоб (`u32` + записи, `0x465400` → `0x466ae0`)
  разобраны и описаны. CTest `events` — 1/1. Коммиты `10ae607`, `4240852`.
- **Пункт 4 (player integrator).** Новый инструмент
  `tools/re/player_integrator.py` выполняет байты оригинала (PE-образ
  `gundx.exe` в плоской памяти, подстановка состояния игрока и кадра
  вызывающего, цикл x87); константы `0.15`/`-0.85` и опкоды цикла
  сверяются с образом — «эмулируем не то» исключено.
  C++-порт и голдены — в `engine/src/player/controller.*` и
  `engine/tests/test_player_controller.cpp` (диадические векторы, совпадение
  побитовое). Проверки: `player_controller` 1/1, полный CTest в worktree
  44/52 (8 падений — те же внешние `level1/2` swap и `KEYCFG`, не код),
  `regress.py` 113/113. Коммиты `05736f8`, `44b9b6a` (merge), `9d1e3c8`,
  `ae09325`.
- Impact-ветки: H1 (нейтральные blast’ы под водой), RGM (`ADD_THING` type 32,
  пустой `impact_blasts`, expire 3), H2 guards (`air/water/ground/DM`; TAC
  NUKE 15 в таблице, по 3 на контекст), hitscan impact-события (CANNON,
  FUSION, MICROWAVE, DISRUPTOR, MESON), EMP `inline_blasts` (5), DISINT
  secondary 400@200; `apply_blast_damage`/`apply_weapon_blast` применяют
  collide/expire к игрокам и THING. Коммиты `7b5a331`, `5c5b158`, `e7bf74a`,
  `3e062c7`; `weapons`/`things` — ok.
- Impact-отчёт прочитан; добавлены `Blast`, `apply_blast`, `apply_thing_damage`,
  исправлен `ShieldSide`, предпочтение воздушной ветки `FIRE_*`.
- `weapons` (1/1), `things` (1/1), `thing_ai` (1/1), полный CTest 52/52
  (до последней правки `thing_type`; targeted после неё — 1/1).
- Добавлен CLI `--third-person`: `TICK` (LIGHT) ставится низом на
  `SET_PLAYER_START`, поворачивается на `spawn.yaw`, камера — `CameraRig`
  `Third` с окклюзией `.sbw`. Флаги `--third-distance/height`,
  `--player-model`, фото-оверрайды `--third-pos/--third-yaw`.
  Новый тест `cli_third_person` — 1/1. Кадры:
  `out/third_person/level0_tick_third_person.png` (1280×720),
  `out/third_person/level1_tick_third_person.png` и
  `out/third_person/level1_tick_third_person_front.png` (1280×720).
  Для `level1` стартовая шахта слишком тесная; шасси поставлено в зал базы
  `(200,0,724)`, камера — `--pos/--look` (см. `docs/CAMERA_CPP.md` §5.3).
- ⚠️ Установка игры изменена по просьбе пользователя: содержимое `level1.*`
  и `level2.*` переставлено. Поэтому corpus-голдены `core`, `core_full`,
  `camera_rig`, `sbtvm`, `vm`, `vm_full`, `things_full` падают на реальных
  данных; `controls` падает независимо (изменённые `KEYCFG/*.key`). Это не
  регрессия кода; после обратного swap голдены снова сойдутся.
- THING/AI/events: `apply_thing_damage(Things&, id)` пишет HP/щиты/статусы
  и постит `damaged`/`destroy`; `Things::tick(..., EventQueue*)` постит
  `expire` при `+0x1e8`; AI сканирует до 8 игроков и вещи, `history_lag`
  даёт `aim_point`, статусы зеркалятся. `EventDispatcher` реализует
  типы 1/2/3/4…12, `SwitchEffect[0x200]`, файл `0x5a4af0`
  (`serialize`/`deserialize`), `install_switches` из `ADD_SWITCH`;
  `accept_network_blob` принимает сетевой блоб; `fire(..., charge)` считает
  скорость снаряда по `0x46f0f0`; AI-события несут `aim_point`/target;
  `Things::spawn`/`remove` дают lifecycle с create/destroy. Коммиты
  `4b380e7`, `f46a96e`, `0d58ecd`, `5265854`, `052f7e0`, `cddf3ce`,
  `c3e4cd2`, `a1cc74b` и др.
- Side-задача (звук переключателя LEVEL1) завершена: SOU2-выход `0x852d80`
  — это 13 поддиапазонов, а не PCM; добавлен `tools/re/render_sou2.py`
  (настоящий синтез `0x8534c0` через `x86mini`), пересобраны
  `out/L01BONUS_switch_music.mp3` и `out/level1_switch_sequence.mp3`,
  README/`docs/FORMATS.md` исправлены. Тональность `L01BONUS` исправлена:
  музыкальный микшер всегда 32000 Гц, заголовочные 16000 — не playback rate.
- По замечанию пользователя повтор на ~60% проверен: это шов игрового
  зацикливания ровно на **2.072 с (69%)**, а не артефакт MP3. Чтобы убрать
  повтор, файл теперь содержит **один цикл** 2.072 с + тишину до 3.0 с;
  `--loop --seconds 3` по-прежнему воспроизводит игровое поведение.
  «Только левый канал» — не потеря: в источнике правый тише на ~23 дБ
  (raw RMS 10127/1199; render 0.269/0.0188), фильтр симметричен. Вставка
  сведена в моно `(L+R)/2`, поэтому слышна в обоих каналах; `SWELEC1` не
  изменялся. SHA и корреляции — в `out/switch_level1_README.md`; правила на
  будущее (32000, шов 2.072 с, правый канал, M/S) — `docs/SOU2.md`.
- Следующий шаг — пункт 1 (не начинать без явного запроса: текущая
  side-задача завершена).

## Последний ход (Ghidra, 4 сабагента + интеграция)

- **`tools/ghidra/mcp.py`** — CLI без зависимостей поверх работающего
  pyghidra-MCP (`decompile/disasm/xrefs/strings/search/bytes/callgraph/raw/
  batch`); `docs/GHIDRA.md` §4.1. Коммит `5a8778e`. Им пользовались все
  сабагенты (проект Ghidra общий, сессии не плодятся).
- **Заряд оружия доказан и портирован**: `0x4453d0` — `player+0x2fa0`
  (`charge`) и `+0x2fa8` (`+= dt*weapon[+0x00]`), спавнер `0x46f0f0` читает
  первое как `speed = TT+0x10 + charge*operand[9]`. `gm::update_charge`/
  `ChargeState` (режимы 0/1/2..6, граница ровно 1.0 не стреляет, авто-выстрел
  только 3/5, гейт по кулдауну `0x445aa2`), `charge_rate` памяти = файл ×
  0.91 (`0x450d34`, `[0x4a2488]`). Реальные зарядные: GRENADE/VIRAL AGENT
  (mode 2, 0.01365/тик). CTest `weapons`. Коммиты `9601d68`, `f5b68cc`.
- **RNG (M2.4)**: `0x480e30` = CRT `rand()` (MSVC LCG), `0x480e20` =
  `srand`, единственный сид `0x44a491` = `time(nullptr)` → оригинал
  недетерминирован; 58 потребителей классифицированы; `0x480e6a` — это
  `fmod`, не RNG (нормализация yaw ±2π). `game/rng.h` + CTest `rng`,
  `docs/RNG.md`. Коммит `e984d08`.
- **M2.3.1** (`9895476` + правки `783fccf`): `docs/PLAYER_FIELDS.md`,
  `tools/re/player_fields.py`; база исправлена на `0x819040`, `0x81bd00` —
  внутренний адрес; census 255 смещений, 114 доказано, покрытие 97.2 %.
- **M2.3.5** (`50c5239`): `docs/WEAPON_FIELDS.md`, `tool/re/weapon_fields.py`;
  карты W/A/E/P сняты store-функциями + push-порядком; найдены расхождения
  с `FORMATS.md` §12.3.
- **M2.3.3** (`44a229b`): `docs/THING_FIELDS.md`, `tools/re/thing_fields.py`;
  типы `0x776d60` шаг `0x3a8`, экземпляры `0x7ac080` шаг `0x528` (260),
  `0x800bb0` — секторные флаги; поправка `SOE +0x010` (`11913de`, с
  `gm/soe.py`, `test_soe`, `regress.py`).
- **M2.2.1** (`2cc6b35`): `docs/INIT.md`, `tools/re/init_flow.py` — цепочка
  WinMain → окно → preinit (bigfile/CFG/CLI/GAMEPAL/DirectSound) → DDraw →
  DInput → DDraw surfaces → тик-поток (Sleep 11 мс, `GetLocalTime`) → цикл;
  101 шаг с байтовой проверкой, headless-минимум.
- Проверки: CTest `weapons`, `rng`, `soe`, `thing_ai` — ok; quick CTest
  39/43 (4 прежних внешних падения: `core`/`sbtvm`/`vm` из-за пары
  level1/level2 и `camera_rig` — голдены level1 не совпадают со сценой);
  `regress.py --category models` 7/7; новые инструменты — `--self-test`
  и `--verify` ok (101/427/30 проверок).
- Дальше: интегрировать `M2.3.2`/`M2.3.4`/`M2.3.7`, затем `M2.3.6`,
  `M2.2.3`, ввод игрока (пункт 8) и `FORMATS.md` (пункт 7).

## Последний ход (закрытие Ghidra-пакета)

- Смержены и проверены все 7 карт: `M2.3.1` (игрок), `M2.3.2` (мир),
  `M2.3.3` (things), `M2.3.4` (двери), `M2.3.5` (оружие), `M2.3.6`
  (сущности), `M2.3.7` (звук), `M2.2.1` (инициализация ✅),
  `M2.2.3` (загрузка уровня). Инструменты: `player_fields.py`,
  `world_fields.py`, `thing_fields.py`, `door_fields.py`, `weapon_fields.py`,
  `entity_fields.py`, `sound_fields.py`, `init_flow.py`, `level_load.py`
  (+ `tools/ghidra/mcp.py`); у каждого `--self-test` и `--verify`.
- Правки кода по картам: заряд `0x4453d0` (+×0.91, события, reset),
  `spawn_player_shot`, `MsvcRand`, dwell/timer двери + `ADD_LIFT`
  speed/height, округление микшера `0x8610d0`, биты игроков сущностей
  `0..7`, `SOE +0x010 = speed`.
- Осталось (в порядке приоритета): реестр «event id → THING_TYPES blast»
  (п.9), `SECTOR_STUFF` (п.10), ввод игрока (п.8), `FORMATS.md` (п.7),
  one-shot `0x200`/дубли id/`SLEEP` (M2.3.6), выгрузка/reload и порталы
  S6/S17 (M2.2.3).
- Проверки на момент остановки: `weapons`, `mixer`, `doors`, `doors_full`,
  `soe`, `rng`, `audio_output`, `game_state` — ok; `vm`, `vm_full`, `sbtvm`,
  `core`, `core_full`, `camera_rig`, `things_full` падают **только** из-за
  внешней пары level1/level2 (goldens сняты с неперепутанных файлов);
  `regress.py --full` — 121/121.
- `8235fd9` — недостающая реализация `update_charge(..., suppress_state_event)`
  (объявление в `weapons.h` и тест были в `dd4f3ae`, тело висело
  незакоммиченным; без него дерево не собиралось).
