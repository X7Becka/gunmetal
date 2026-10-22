# JOURNAL — журнал ходов

История отчётов, перенесённая из `NEXT.md` 2026-10-22 (новые записи — сверху).
Очередь и следующий шаг остаются в `NEXT.md`; узкие места и очередь задач —
`docs/RE_AUDIT.md`. Факты об игре — в профильных `docs/*`, не здесь.

## Последний ход

## Последний ход — палитра, индексы текстур, четыре сабагента (2026-10-22)

- **Палитра движка найдена и подключена** (`M4.1.7`): `0x44af88` →
  `0x47efe0` с аргументами `"frontend"`/`"gamepal"` и приёмником `0x83d320`,
  публикация `[0x83d2c4] = 0x83d320`, чтение `[reg+0x83d320]`, строка ошибки
  `0x4abbb8`. Носитель `FLAG/naUSA.BMP` держал magenta в 21 слоте
  (0, 173..175, 191, 240..255) — отсюда «magenta/шахматка». Python
  `gm/palette.py::from_archive`, C++ `res/bmp.cpp::engine_palette`;
  fallback на BMP остался. `docs/PALETTE.md`.
- **Индекс текстуры полигона — 1-based** (`tex_names[i-1]`) в `gm/render.py`
  и `render/render.cpp`. Доказательство из данных: `value[i-1]` (счётчик
  ссылок записи) == число полигонов с индексом `i`, 11/12 точно (ALUMNM4
  +3 overlay); 0-based не даёт совпадений.
- **Голдены пересчитаны** через `tools/re/render_goldens.py` (теперь и
  модели, и морф) и `tools/parity_mip.py`; `engine/tests/test_main.cpp` —
  9 констант. Зелено: CTest `core`, `sprite`, `parity_mip` 50/50,
  `regress.py --mirror` 113/113, `--full --mirror` 122/123 (единственное
  падение — внешний `install / manifest audit`: в установке изменены
  `KEYCFG/*.key`, `docs/BASELINE.md`).
- **Сабагенты (4 ветки влиты)**: `feat/lmaudit` — покрытие лайтмапов 40.6 %
  граней `level0`, страницы mean 30/127, 29 % нулей, brightness без
  лайтмапа mean 33.5 (74.7 % ≤ 32); `feat/texaudit` — 377/378 текстур
  резолвятся, magenta только от палитры-носителя (0.0004 % на движковой);
  `feat/oraclerender` — воспроизводимый рендер кадра из sidecar + метрики;
  `feat/spritegolden` — постоянный Python-эталон спрайтов, 7/7 совпадений,
  `--palette carrier` даёт старые 7 хешей (значит изменилась только палитра).
- **Бесплатные модели кончились**: `opencode-go/beam-free` недоступна в
  стране, `opencode/*-free` — «Free usage limit reached». По разрешению
  пользователя партия шла на `opencode-go/deepseek-v4.1-flash`.
- **Эталонные кадры**: после правок `win1` совпадает по контурам
  (edges 0.43), яркость нет (corr 0.02) — следующий шаг `M4.2.5`
  (кривая `0..127 → RGB` через `LOOK16.TAB`); `capture.ps1`/`state.ps1`
  надо объединить в один снимок (`docs/ORACLE.md`).


- **«Кадр 1.875» у `ADD_SWITCH` закрыт** — это не кадр. Операнды 4..6
  полиморфны: `kind 0` (531 шт.) — мировые `x/y/z`, `kind >= 0x101` (3433:
  `0x101` 1855, `0x102` 1067, `0x103` 131, `0x104` 380) —
  `(sector, object, polygon)`. Доказано: `0x44fc30` для kind 0 сравнивает
  мировую позицию игрока с операндами напрямую; ветка `0x101` берёт из луча
  `0x43c51f` три `int` и требует точного совпадения тройки и `t <= radius`;
  `0x43577c` превращает тройку в точку; `0x44fb30` (вызов из **опкода 75**)
  считает евклид до полигона; `0x44fad0`/`0x44fbd0` — точные тройки;
  опкод `0x24` `SET_SOURCEPOLY_SWITCH_EVENT` (`0x44e680`) переписывает
  `target` по полигону источника. Данные: 531/531 позиций внутри бокса,
  3433/3433 троек существуют. `gm.scene.SCALE` удалён, `tools/re/doors.py
  --frame` разделён по kind, `regress.py` сверяет с мировыми вершинами.
- **Триггеры-полигоны в движке**: `WorldState::poll_aim` (`0x44fc30`,
  `t <= radius`), `poll_blast` (`0x44fb30`, `d <= radius`; гейт `0x428399`
  = `(object.flags & 0x10000) == 0` доказан, но не подключён),
  `trigger_poly` (`0x44fad0`, только точная тройка), `Switch::is_poly()`;
  CTest `test_polygon_triggers` (граница `<=`, промахи, изоляция видов,
  реальный dm1 124/5). `0x102`-exact и гейт флагов — остаток.
- **События дверей**: `post_door_events` — `ids[3]`/`ids[4]` и crush
  постится как **тип 1** (`0x4795b0`: `id`, `param = sector`, `a = object`,
  origin, `extra = -1`), курсор как у switch-записей, хвост переживает
  полную очередь. Crush исправлен на фиксированный `-22` (было `ids[4]`).
  `GameRuntime::tick` зовёт его сразу после `post_switch_events`.
- **VM: `SET_SOURCESECTOR_*` (27–31) и `SET_HANDLESECTOR_*` (40–45)**.
  `0x457fc6` пишет `0x800b60 + edi*0x58`, где `edi` — сектор, с которым
  вызван `0x457390` (третий аргумент, из события) → `VmState::source_sector`.
  `0x45829b` ищет операнд-хэндл в `0x5b1df8[0..0x5e3de0)` и берёт сектор из
  `0x617fb8` → `VmState::sector_handles` + `vm_load_sector_handles`
  (`SECTOR_HANDLE: <ld> <ld>`, `0x4b9888`, до `0x100`). Таблица **не строится
  из уровня** (`0x456f50` обнуляет, `0x463820` читает текст), поэтому в
  поставочных уровнях `SET_HANDLESECTOR_*` — no-op, как в оригинале.
  `BRIGHTNESS/TYPE` по-прежнему пишут запись сектора `.sbw` (событие + note).
- **Тип 18 (`0x476830`, push) разобран и реализован**: обход восьми игроков,
  полоса `|dz| < box[2] + player+0x2d80`, планарный октагон
  `< box[1] + player+0x2d68`, `step = min(reach − d, dt·speed)`, толчок
  своей скорости по доминирующей оси (`+0x1b8` x / `+0x1c0` y), событие
  `+0x3f8` при `step > 0.1` и `+0x3f4 > 0`. CTest `test_push`.
- **Гейт `0x428399` в blast-триггере**: `poll_blast` пропускает попадание,
  если у объекта в `+0x70` стоит бит `0x10000` (функция возвращает ровно этот
  бит, `0x44fb63` требует ноль); прицельный луч такого гейта не имеет.
  `PolygonHit::object_flags`.
- **Прицельный триггер в игровом слое**: `GameRuntime::aim_switch(player,
  direction, max_distance = 10000)` — луч из глаза игрока
  (`set_player_eyes`) через `CollisionWorld::raycast`, тройка из
  `RayHit::triangle` (`sector`/`object`/`source_poly`), длина `hit.t`; далее
  `poll_aim` + `post_switch_events`, как у `press_switch`. CTest `aim_switch`
  (стена `x=100`: попадание, `max_distance`, радиус, чужие kind,
  детерминизм 10 кадров). **Луч из камеры**: `0x44fc30` берёт направление из
  базиса `0x41e025` (второй выход = forward), C++ —
  `GameRuntime::aim_switch_from_camera(player, roll, pitch, yaw, max_distance)`
  / `aim_switch_from_basis(player, basis, ...)` поверх `gm::camera_basis`
  (порт `0x41e025`); ручной `aim_switch(direction)` остался. Дальность
  оригинал не клампит (`0x43c51f` стартует с double `1e38`), принимает по
  радиусу switch (`+0x08`). CTest `aim_from_camera` (`+X` при `yaw=+π/2`,
  промахи по yaw/pitch, roll не влияет, NaN/чужой игрок → 0).
- **Режим 16 (`0x476070`) доработан**: `+0x210/+0x214` типа
  (`0x46dd71/0x46dd77`, база `0x776d60`) → `Thing::timer_period/timer_step`;
  `+0x3e8` флипает направление перебора полос на каждом выстреле, а
  `rand() % +0x3ec == 0` добавляет `+0x3f0` к кулдауну. Розыгрыш — opt-in
  через `ThingMotionInput::rng`/`Things::set_motion_rng` (`MsvcRand`),
  без ГПСЧ шаг детерминирован. CTest `test_timer_fields`,
  `test_patrol_scan_and_timer`.
- **Смержены три ветки сабагентов** (все на бесплатных моделях, без push):
  * `b0587a2` door-audio: очередь `SoundRequest` для дверей и switch
    (`WorldState::drain_sounds/pending_sounds/clear_sounds`, курсор без дублей,
    лимит 256, `sounds_queued/dropped`); доказаны форматы `0x46b8d0`/`0x46b840`/
    `0x46b9f0`/`0x46ba90`, call sites `0x44f776/0x44f66b/0x44f5ee/0x44f4b1/
    0x44f53e/0x44f5a6`, `+0x60` = attenuation 0.03; **crush звука не даёт**;
    лифт и центроид — следующий пункт.
  * **Hitscan-луч и конус цели** (merge `e082c26`): `cast_hitscan` поверх
    `CollisionWorld::raycast` с исключительной границей дистанции (как
    `0x477f90`) и `HitscanHit` (point/normal/distance/sector/object/polygon/
    triangle); конус `0x477f90`/`0x4779f0` — проекция на строки матрицы
    (`0x437909`), глубина строго > 0.32 (`0x4a2658`), строгие боковые
    сравнения, масштабы `0x81c020/0x81c028/0x81c030`, `hitscan_cos_limit`,
    `hitscan_scale_spread`. CTest `test_hitscan_ray`/`test_hitscan_cone`.
    `GameRuntime::resolve_hitscan_impact` — мост к exact-switch `0x44fad0`
    (kind `0x102`) и `impact_event`; `ShotResult` несёт тройку
    `sector/object/polygon`. CTest `hitscan_impact_switch`.
    `Weapon::hitscan_max_distance` — первый операнд FIRE (MG 9999, CANNON
    9000), `fire()` ограничивает им луч; `update_player_weapon` сам зовёт
    `resolve_hitscan_impact` и кладёт результат в `PlayerWeaponTick::impact`. Осталось: выбор жертвы/урон по
    сущностям (`0x478520`/конус), вызов из игрового выстрела.
  * **Выбор остановки лифта по `0x44f690`**: три alias-кнопки
    (`call_ids[0]` — следующая с переносом, `[1]`/`[2]` — вверх/вниз с
    зажимом), совпавший `floor_id` перекрывает выбор, пустой выбор молчит;
    `lift_targets_` включает floor-id с дедупом. CTest
    `test_lift_call_aliases`.
  * **Звук лифта в очереди** (в master): `0x44f690`/`0x4502a0` → `0x46b840`
    (`n_a/v_a` вызов, `n_c/v_c` прибытие), `0x46b960` (петля `n_b/v_b`,
    class `2000000+индекс`), `0x46ba90` (стоп петли), событие прибытия
    `0x4795b0` с id из третьей колонки остановки
    (`WorldEvent::LiftArrived::event_id`); позиция — origin кабины
    (`lift_transform`). `LiftMove` отдаётся один раз на движение,
    `LiftMoveStop` — на прибытии; сейв с уже едущей кабиной догоняет петлю
    на первом тике. Центроид полигонального switch (`0x43577c`) считается
    в `WorldState::build` (`Switch::world_position`). CTest
    `test_lift_sound_queue`, `test_polygon_switch_centroid`.
  * `228546b` vm-light: `VmState` реально хранит `world_sectors`
    (`kind = тип×6`, `light`), `world_polys` (texture/material/scroll/brightness),
    `poly_handles`, `source_object/source_poly`; загрузчики `vm_load_world`
    (из `.sbw`) и `vm_load_poly_handles` (`POLY_HANDLE:`, max `0x1f4`),
    семантика «все совпадения» у handle-таблиц. Осталось: сдвиг UV по
    игровому таймеру `0x5a46e4`, `SET_SECTOR_LINK` — только проверка.
  * `deaea0a` ai-aim-tick: `GameRuntimeConfig::ai_apply_aim` (opt-in) вызывает
    `Ai::apply_aim` сразу после AI-фазы (2a), `GameRuntimeStats::aimed_things`;
    при включении рантайм сам делает `Things::set_motion_enabled(true,
    &collision_)` (углы тела живут в состоянии автоматов).
- **Кулдаун оружия AI**: хвост `0x473a00` — `rand() % +0x3ec == 0` добавляет
  `+0x3f0` к кулдауну; в C++ `AiConfig::use_weapon_timer` +
  `Ai::set_weapon_rng` (`Thing::timer_period/step`), CTest
  `test_weapon_timer_bonus`.
- **Таблица детей типа + спавн `0x46d000`** (merge `f96ebfc`):
  `Thing::child_types/child_params/child_offsets/child_angles` из
  `THING_TYPES+0x300/+0x310/+0xd8+0x18i/+0x138/158/178` (count `+0x2fc`,
  clamp 4), тест `test_child_table`; на корпусе 591 экземпляр с детьми,
  clamp/парность массивов проверяются на всех 36 уровнях.
  `Things::spawn_children()` — один уровень детей, позиция
  `origin + basis·offset`, композиция ориентации `0x41e141`/`0x41e2f9`,
  флаги `тип | child_param | param_12`, create-event `+0x1f0`, родитель
  получает id детей. Пока **opt-in** (`ThingsOptions::spawn_type_children`,
  по умолчанию false): включение сдвигает golden-счётчики 7 617/591.
  CTest `test_type_children`, `test_type_children_real` (level11: 37 детей
  у 293 инстансов). Осталось: включить по умолчанию и пересчитать голдены;
  source-relative родитель остаётся с нулевым origin (нет мировой позиции);
  порядок id детей отличается от оригинала (ребёнок сразу за родителем).
- **Луч прицела из камеры**: `GameState::aim_direction()` =
  `camera_basis(angles[1], angles[0], angles[2]).forward` — готовый вход для
  `GameRuntime::aim_switch_from_basis()` (CTest `test_aim_direction`).
- Итог: **`ctest` 61/61** после M0.8 content-addressed путей (ранее 52/59;
  `core_full`/`things_full` больше не падают на переставленных
  `level1/level2`); `regress.py --full --mirror` 121/121.

- **Трансформ `.sbw` исправлен** (`feat/sbw-transform`): заголовок объекта —
  **12 × `f64`** (origin + базис), в поставке базис единичный; множителя
  `1.875` нет (это float-биты старшего dword от `double` 1.0). Доказано:
  загрузчик копирует 96 Б без конверсии (`0x42ae86`), рантайм читает
  `FLD double` (`0x437af8`), 338 304/338 304 младших dword = 0, базис
  единичный 28 192/28 192, AABB сектора покрывает вершины 4716/5104 (в
  старом кадре 0/5104), `saved0.sbw` (записан движком `0x42afbb`) побайтово
  равен `level0.sbw`. Согласованность со `.sbt`: спавны внутри мира 36/36
  (было 21/36), зазор до пола 13.7…258.1 (медиана 56, было 570), спрайты
  ближе 5 ед. 303/547 (было 15/547).
  Исправлены `gm/sbw.py`, `render/collide.cpp`, `render/render.cpp`,
  `gm/scene.py` (`VOLUME_SCALE = 1.0`), `gm.py`, `regress.py`, голдены
  `collide`/`render`/`player_controller`/`cli_third_person`. Новые тесты:
  `sbw_transform`; новые инструменты `tools/re/collide_goldens.py`,
  `tools/re/render_goldens.py`. **`test_thing_motion` переведён на `f64`**
  (синтетические заголовки), иначе 16 падений.
  `docs/FORMATS.md` §10 и «Кадр координат» переписаны.
  **Открытый вопрос:** операнды 3..5 `ADD_SWITCH` численно равны
  `1.875·local` (без `origin`), хотя `0x44fc30` сравнивает их с позицией
  игрока напрямую; `gm/scene.py::SCALE` оставлен только для проверок
  переключателей/дверей. Позиции света/звуков в кадр уровня не укладываются.
- Итог прогонов после всех слияний: `ctest` 52/59 (7 падений — прежние
  внешние `core`, `core_full`, `camera_rig`, `things_full`, `sbtvm`, `vm`,
  `vm_full`), `regress.py --full` 121/121, `parity_mip.py` 50/50.

- **Двери/лифты: геометрия, триггеры, звук, writer** (`feat/doors-motion`).
  Движок не трогает вершины — он переписывает трансформ объекта: тик
  `0x44f2e0` (`phase += step·dt·state`, clamp `0..0xffff`, делитель
  **`1/65535`**), смещение `0x434f72` пишет `obj+0x90/0x98/0xa0` (вектор за
  кадр, его читают `0x437d05`, `0x434fed`, `0x440867`), коммит `0x44f3f0`
  (события `ids[3]`/`ids[4]`), трансформ `0x44dfc0` → `0x434887` (единственный
  писатель origin/базиса): `center = closed + (open-closed)·frac`,
  ось `z→x→y`, базис = поворот на `-angle`, `origin = center − basisᵀ·size`.
  У 1422/1779 дверей все `A = 0` (чистый сдвиг), у 357 есть угол.
  Коллизия в проёме — `0x450590` (kind 2 — реверс, kind 3 — crush +
  `event_post(-22)`). Триггеры: `0x44fc30`/`0x44f960`, метрика
  **`0x4344df` = max + (mid+min)/4**, защёлки нет. Звук: три пары
  `+0xa0/+0xac` (пуск), `+0xa4/+0xb0` (движение), `+0xa8/+0xb4` (стоп);
  лифт — `+0x84..+0x98`. `basis` кабины — 6 `f64` (углы + приращения),
  `+0x48` — приращение позиции.
  Writer `res/sbt.{h,cpp}`: `SbtSwitchRecord`/`SbtDoorRecord`/`SbtLiftRecord`
  + `parse/encode_*_block`; round-trip `saved0`/`saved1` 25 дверей / 24
  переключателя, 45 лифтов / 151 остановка — байт-в-байт.
  **Исправлено `ADD_LIFT`**: операнды 9..16 — `double` (6 `f64`), поэтому
  `speed = head[20]`, `height = head[22]` (старое `head[16]/[18]` читало
  половину `f64` — скриптовые лифты стояли). Тесты `doors`/`doors_full`
  (61 дверь `dm1` по геометрии) зелёные.

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
