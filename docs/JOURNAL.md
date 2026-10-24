# JOURNAL — журнал ходов

История отчётов, перенесённая из `NEXT.md` 2026-10-22 (новые записи — сверху).
Очередь и следующий шаг остаются в `NEXT.md`; узкие места и очередь задач —
`docs/RE_AUDIT.md`. Факты об игре — в профильных `docs/*`, не здесь.

## Последний ход

## 2026-10-23 — SECTOR_STUFF: какой слот адресуют 27–31 и 40–45

- Влита `feat/sectorstuff`: новый `docs/SECTOR_STUFF.md` и
  `tools/re/sector_stuff.py` (`--self-test` 33/33, `--summary` 36 уровней за
  1,6 с, `--level`/`--map`/`--json`).
- Доказано дизасмом: слот `SECTOR_STUFF` = `index*11*8` (`0x457fdd..fec`,
  `0x4582c6..cc`), запись сектора мира = `index*0x18` (`0x42d8c0`).
  Для 27–31 индекс — «сектор источника» (`edi` = арг. 3 `0x457390`,
  `0x4573ab`), он приходит из `SLEEP` (`0x45c4a5`), `GENERATE`
  (`0x4575bb` → `0x45fa00` → `0x45fac2`) и событий сектора (`0x445095` →
  `0x4795b0` word `+5` → `0x479a3a`). Для 40–45 — `0x617fb8[i]`, где
  `0x5b1df8[i] == E` (`0x4582bc`).
- Данные: 1593 команды (570 source / 1023 handle); хэндл разрешается
  статически 1017/1023 (6 тегов отсутствуют в таблице — no-op оригинала);
  источник: 16 известен (`level16` 133..136), 2 неоднозначны, 552 неизвестны
  (нужна событийная трасса). 27/29/31/44 в поставке не встречаются.
- **Опровергнуто:** `SECTOR_HANDLE` **не** пуста — запись `.sbt` типа 17
  (`0x463900`: `0x5e3de0`/`0x5b1df8`/`0x617fb8`) есть в 33/36 уровней,
  900 строк; `POLY_HANDLE` — 35/36 (4277), `OBJECT_HANDLE` — 29/36 (207).
  Исправлены `docs/RECORDS.md` §2.5, `docs/VM_EXEC.md` §3.2,
  `docs/LEVEL_LOAD.md` §4 (тип 17 вёл на колонку полигонов `0x5e5680`).
- **Опкод 12 в C++** писал `+0x48 ← V`, `+0x4c ← N`; в оригинале наоборот
  (`0x4579fb`/`0x4579f6` → push `0x457a04`/`0x457a03`; helper `0x46b740`).
  Исправлено + тесты; `45` был верен (его push-порядок тот же, что у `12`).
  `vm_state_from_level` теперь засевает `VmState::sector_handles` из
  `level.sector_handles` (до `0x100`), иначе 40–45 оставались no-op.
- Проверено: `sector_stuff.py --self-test` 33/33; `gm_vm_tests` rc=0 (в т.ч.
  новый тест `level0` → 26 строк `SECTOR_HANDLE`); `regress.py --category vm
  --mirror` 4/4; `ROADMAP` `M5.2.7`, `INDEX` пересобран.

## 2026-10-23 — loadbar вместо тишины в тяжёлых прогонах

- `tools/loadbar.py` — свой лоадер без зависимостей: одна перерисовывающаяся
  строка на tty, отметки по 10 % при перенаправленном выводе; `%`, `n/N`,
  скорость, ETA, текущий файл, счётчики ok/fail, режим «total неизвестен»
  (бегущий блок + счётчик). `GM_PROGRESS=0|1|log`, `GM_PROGRESS_ASCII=1`.
  Самотест `--test` (9/9) и `--demo` (10 состояний).
- Подключён туда, где раньше была тишина: `regress.py` — живая строка на весь
  прогон (`?? 37 checks ok 37 42s <последняя проверка>`), обход 36 уровней в
  `_pool_map` и четыре параллельных извлечения в `check_many`; `gm.py` —
  `textures` (1648 записей), `sprites` (179), `sounds` (16 чанков по 64);
  `tools/re/gmcache.py` — stat-обход 4215 файлов (бегущий бар) и копирование
  зеркала. Бар флашит stdout перед отрисовкой, иначе при `2>&1 | tee`
  block-buffered stdout и небуферизованный stderr перемешивали строки.
- Проверки: `tools/loadbar.py --test` 9/9; под pty бар живой (44 кадра за 5 с
  на `gm.py textures`, 168 кадров за 16.7 с на синхронизации зеркала, 23 кадра
  за 10 с на `regress.py --category scene`); `regress.py` 113/113 за 73 с (без
  изменений в счёте); `--full` 122/123 за 122 с — единственное падение
  `install / manifest audit` (`modified 3`: `KEYCFG/PERFECT.key`,
  `SPLIT1.KEY`, `SPLIT2.KEY`), то есть известное внешнее расхождение установки
  (`docs/BASELINE.md`, «Внешние расхождения установки»), не связанное с
  правками; `--category extract` 4/4 за 9.2 с; `--category scene` 15/15 за
  9.9 с под pty (бар перерисовывается одной строкой).
  Документация: `docs/REGRESSION.md` §3, `docs/TOOLING.md` §6.

## Ход — камера, UV, HUD, ВМ-сцена (2026-10-23)

- **Камера эталонных кадров.** `pitch` камеры = `p[0x18] + p[0x2E60]`
  (`docs/CAMERA.md`), а `state.ps1`/`capture.ps1` читали только `+0x18`.
  Подбор (`tools/re/oracle_fit.py`, новый) дал по кадрам `win1` 0°, `win2` +8°,
  `win3` +14° и рост `corr` 0.05→0.29. Скрипты читают 0x2E68 байт и пишут
  `camera_pitch`; `oracle_render.camera_frame` его использует. `win1` не
  сводится с `level0` ни углом, ни сдвигом ±200 — помечен непригодным.
- **Вертикальный фокус.** `133.33` выигрывает по `edges` 3/3 и `corr` 2/3 →
  дефолт подтверждён (согласуется с `+0x94 = zoom·4H/(3W)`).
- **Текстурная проекция (доказано).** `texel = (dot(u_axis, v−centre)·64 +
  uv_u_fp) >> 6` — делитель 64, один тексель на юнит; лайтмап — тот же
  числитель `>> 11`. Полигон с `texture == 0 && handle == 0` **не рисуется**
  (setup `0x9fde70`). Реализовано в Python (`uv="texel"`,
  `skip_untextured`) и C++ (`RenderOptions::texel_uv/skip_untextured`,
  `--uv-texel`/`--skip-untextured`); **паритет байтовый** — CTest `core`
  пингует FNV Python-кадра (`0xa914848d24650304`) и совпадает.
- **Материалы.** Второй проход на `win2/win3` побитово ничего не меняет
  (md5 совпадают), LUT кадровый проход не читает, прозрачность — хрома-кей по
  индексу 0 (`docs/ALPHA.md`); добавлен `chromakey`/`Texture::keyed`.
- **HUD.** `docs/HUD_AUDIT.md`: 23 элемента кокпита, 21 кластер по кадрам,
  координаты и поля состояния; в Python пока не рисуется.
- **ВМ-сцена.** `docs/VM_SCENE.md`: 36/36 уровней — сцена, вещи (7 655),
  переходы (66) совпадают со статикой; `M5.2.1`/`M5.2.2`/`M5.3.3` закрыты.
  Побочно исправлены три ошибки `docs/FORMATS.md` §15 (условие хабов,
  перепутанные `level1`/`level2`, `[DM]` = `DEATHMATCH_GAME`) — после правки
  расхождений 0, `M5.3.4` закрыт.
- **Roadmap** сверен с кодом и тестами: 71 ✅ → 82 ✅, 105 ⬜ → 75 ⬜.
  `M5.1.1`–`M5.1.4`, `M5.3.1`, `M6.1.1/1.2/1.3/1.6/2.1/2.2/2.3/2.5`,
  `M6.3.1`–`M6.3.4`, `M6.4.1`–`M6.4.4`, `M3.3.1/M3.3.2`, `M4.4.1`–`M4.4.4`.
- Проверки: `regress --full --mirror` 122/123 (внешний `install_audit`:
  изменены `KEYCFG/*.key`), CTest 60/61 (тот же `controls`), self-test'ы
  новых инструментов: `oracle_fit` 10/10, `alpha_audit` 22/22, `sky` 43/43,
  `fog` PASS, `texuv` 27/27, `hud_audit` 24/24, `vm_scene_check` 10/10.

## Последний ход — свет, материалы, `capture.ps1 -State` (2026-10-23)

- **Кривая света доказана** (`docs/LOOK16.md`): `LOOK16.TAB` = 262144 Б (64³),
  индекс `(r<<12)|(g<<6)|b`, это точное разбиение Вороного 6-6-6 по метрике
  `0x4228e0` (0 расхождений из 262144; евклид 79.8 %, вес 2-4-3 87.6 %).
  Свет ложится линейно `c·L/127` (L=127 = исходный цвет); `c·L/64` и гамма
  опровергнуты. Значит `f = light/127` в `gm/render.py` верна.
- **Яркость кадра сходится**: ours/oracle по world-полосе 0.90/1.01/1.05
  (`win1..win3`). «Тёмный рендер» был следствием magenta-палитры и рассинхрона
  состояния. Остался фог `lighttable` (`0x4409b8` → `0x44194d`, `0x3f−ftol(z)`,
  clamp 4..63) — `M4.1.6`.
- **Скриптовый свет** (`docs/SECTOR_LIGHT.md`): 903 команды на 36 уровнях, все
  в event-обработчиках; при загрузке `sector+0x16 = 0` (поля в `.sbw` нет,
  `SECTOR_STUFF` типа 8 нет ни в одном `.sbt`) → статический кадр =
  `brightness + лайтмап`, второй слагаемый не нужен.
- **Материалы** (`docs/MATERIAL_BLEND.md`): точные LUT-формулы сняты
  (`reflective/translucent` = `(t+s+1)>>1`, `add` = `min(63,t+s)`, `shade` =
  `t·(63−s)/63`, `varadd` — вес от `max(shade)` с разрывом на m=16), но
  потребителя LUT в xref-скане нет, но `0x4409b8` — программный растеризатор
  (8-битный кадр `[ctx+0x104]`), и он использует `material[0]+0x10` как
  таблицу «уровень × индекс» (`L = clamp(0x3f − floor(глубина), 4, 63)`).
  Читает ли LUT 3dfx-путь (`0x437xxx..0x43dxxx`) — не доказано; гейт «второй
  проход» = 300 полигонов из 235 069, бит 0 только у `reflective`. Вьюверный
  `face_blend` — приближение, менять его надо после измерения на кадре
  (альфа текстуры + фог).
- **`capture.ps1 -State`** (`2ae7d6b`): PNG + sidecar одним процессом, два
  сэмпла игрока вокруг `CopyFromScreen`, `state_stable`, свои хэши уровня.
  Проверено парсером PowerShell 5.1 (0 ошибок) и прогоном до проверки процесса.
- Проверки: `regress.py --mirror` 113/113, `--full --mirror` 122/123 (внешний
  `install_audit`: изменены `KEYCFG/*.key`), CTest 60/61 (тот же `controls`).

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


## Перенесено из `NEXT.md` (2026-10-23): партии 3–4 и инфраструктура

История ходов; очередь — `NEXT.md`, канон раскладки — `docs/LAYOUT.md`.

### Сабагенты: статус (2026-10-23, партия 3 закрыта)

- Влиты: `feat/texuv` (`904fadb`, `docs/TEXUV.md`), `feat/hud`
  (`3509a0e`, `docs/HUD_AUDIT.md`), `feat/vmscene` (`92292fd`,
  `docs/VM_SCENE.md`), плюс раньше `look16`/`sectorlight`/`matblend`/
  `alpha`/`sky`/`fog`.
- Итог партии 3: текстурная проекция и пропуск полигонов без текстуры
  **доказаны и реализованы с двух сторон** (`uv="texel"`/`skip_untextured`,
  `--uv-texel`/`--skip-untextured`); HUD-инвентарь (23 элемента) готов;
  ВМ-сцена сверена 36/36, `M5.2.1`/`M5.2.2`/`M5.3.3`/`M5.3.4` закрыты,
  в `docs/FORMATS.md` §15 исправлены три ошибки.
- Модель — `opencode-go/deepseek-v4.1-flash` (бесплатные: `*-free` —
  «Free usage limit reached», `beam-free` — «not available in your country»).

### Партия 4 — в работе (2026-10-23)

Три агента (модель `opencode-go/deepseek-v4.1-flash`):

- `feat/metric` — `tools/re/frame_metric.py` + `docs/FRAME_METRIC.md`:
  метрика кадра, устойчивая к шуму текстуры и сдвигу на 1–3 px, с
  калибровкой на `win2`/`win3` для четырёх вариантов рендера;
- `feat/hudcompose` — `tools/re/hud_compose_ref.py` + `docs/HUD_COMPOSE.md`:
  эталонная композиция кокпита 320×78 по `docs/HUD_AUDIT.md`, сверка с
  кадрами, план переноса в `gm/render.py`;
- `feat/equip` — `tools/re/equipment.py` + `docs/EQUIPMENT.md`: модель
  12 оборудований (`M6.2.4`), активация/эффекты/слоты.

Пока они работают, влито и проверено: движковая текстурная проекция с
**байтовым паритетом** Python↔C++ (CTest `core` пинит FNV
`0xa914848d24650304`), `--pitch-offset` в `oracle_render.py`, `M2.3.4`,
`M3.3.1`, `M3.3.2` закрыты (roadmap 83 ✅ / 74 🟡 / 74 ⬜).

### Следующая партия (когда понадобится)

1. **HUD в Python** — по `docs/HUD_AUDIT.md` (23 элемента, координаты и поля
   известны): панель + текст `0x47faf0` + цифры; тогда полный кадр сравним.
2. **Переключение дефолтов на движковые** (`uv="texel"` + `skip_untextured`):
   перебазировать голдены Python/C++ и `parity_levels`, обновить FNV-пины.
3. **METRIC:** заменить `corr` на метрику, которая не награждает «крупу»
   (например совпадение по Sobel-краям с допуском сдвига), иначе решения по
   UV/полигонам принимаются против визуального результата.

### Сабагенты: статус (2026-10-23)

- Партия 2 влита: `feat/look16` (`9d47865`), `feat/sectorlight` (`67f3d3e`),
  `feat/matblend` (`6282101`). Бесплатные модели по-прежнему недоступны
  (`beam-free` — не в этой стране, `opencode/*-free` — лимит); запуски шли на
  `opencode-go/deepseek-v4.1-flash` по разрешению пользователя.
- Worktree: `~/build/gm-sub-{look16,sectorlight,matblend,spritegolden,
  texaudit,oraclerender,lmaudit}` — можно удалять после мержа.

## Инфраструктура (2026-10-22, ✅)

- **Постоянный кэш вместо tmpfs.** `tools/re/gmcache.py`: листинг objdump,
  бинарный индекс `gundx.asm.idx`, копия exe и зеркало установки в
  `~/.cache/gunmetal-re` (переживает `wsl --shutdown`). `exelib` собирает
  листинг сам; `disasm()` — bisect + seek (≈100 мс → ≈0,02 мс на запрос),
  `gmcache.py selftest` — 98 сэмплов, 0 расхождений на 283 626 строках.
- **Зеркало игры.** `regress.py --mirror` (сверка + автоСинхронизация):
  `113/113 за 45,7 с` против `113/113 за 78,8 с` на `/mnt/d`;
  `gm.py inventory` 7,1 с → 0,14 с, `sob --check` 1,5 с → 0,07 с.
- **Регистр имён.** `regress.py::gp()` и пары `.SME/.SML` больше не зависят
  от регистронезависимой drvfs (зеркало на ext4); `savegame.py` берёт exe из
  кэша, а не из `/tmp/opencode` (из-за этого прежний baseline давал 112/113).
- **Ещё параллель.** `gm.py sounds` 28,6 → 9,6 с (WAV-деревья совпали
  побайтово), `sou1_model.py --verify` ~10 мин → ~2 мин (`--jobs`, JSON
  совпал с `docs/manifests/sou1-fnv.json`, 0 расхождений).
- **Кэш результатов.** С `--mirror` четыре чистые тяжёлые проверки
  (`sob_writer`, `sou_container`, VM-корпус, film parity) кэшируются по хэшу
  исходников + digest зеркала + argv; повторный `--full` 106,9 → 79,8 → 60,2 с
  (`--json.cached`), падения не кэшируются.
- **Тесты по содержимому.** `tools/re/game_content.py` (+ `engine/tests/
  game_content.h`, `game_path.h`): `.sbt`/`.sbw` резолвятся по FNV-1a64
  канонического содержимого из манифеста, поэтому переставленные в установке
  `level1`/`level2` больше не дают ложных падений (`core`/`camera_rig`/
  `sbtvm`/`vm` зелёные). `--check` фиксирует аномалию, `--write` обновляет
  таблицу детерминированно.
- **DOS-сборка `gun.exe`.** `tools/re/lexe.py` — LE/LX-парсер (stdlib):
  7 объектов, 670 страниц, 33 055 fixup на 200 страницах, импортов 0,
  EIP `obj 2 + 0x8f614` → файл 0xD9E14 (`eb 76`, баннер WATCOM), 670 страниц
  покрывают объекты без пересечений; `--selftest` 23/23, 10 порч → ValueError.
  Осталось: iterated pages объекта 7, неопознанные поля заголовка (`docs/LEXE.md` §8).
- **Аудит установки.** `tools/re/install_audit.py`: 2161 игровой файл,
  2154 ok, 6 renamed (3 пары `.sbc`/`.sbt`/`.sbw`), 0 missing/modified/extra,
  `GUNMETAL.CFG` volatile; 4,3 с на 8 потоках, `--strict` → rc=2.
- **Фазз-корпус.** `tools/fuzz_formats.py --save/--replay/--seed` + инспектор
  `tools/qa/fuzz_corpus.py`: 552 кейса quick / 1224 полных, манифест с sha256
  входов, детерминизм проверен (`docs/FUZZ_CORPUS.md`).
- **Покрытие.** `tools/qa/coverage.py` (stdlib `trace`): быстрый набор — 41,8 %
  строк (`--full` — 47,7 %), тёмные места `render`/`sou1`/`sbv`/`png`/`sml`;
  `--json`, `--missing MODULE`, `--min` для гейта.
- **Контекст/токены.** `tools/ctx.py` (`doc`/`addr`/`find`/`outline`/`task`,
  бюджет 40 строк), `docs/INDEX.md`, правила в `AGENTS.md`, методика —
  `docs/RE_METHOD.md`.
- **Проверено:** `regress.py` 113/113 на установке и на зеркале, `--full`
  121/121 за 175,7 с (зеркало),
  `player_fields --self-test`, `names`/`table_layout`/`xref`,
  `gmcache selftest`, `ctx.py index`.
- **Следующий шаг:** пункт 1 — освещение `level0` (кривая `0..127 → RGB`
  через `LOOK16.TAB`). Блокеров нет.


## Ход 2026-10-23 (эффективность + архитектура + HUD-проверка)

- Влит `feat/texuv` (док `docs/TEXUV.md` и `texuv.py` жили только на ветке,
  хотя ROADMAP на них ссылался): `texuv.py --self-test` 27/27, 23 707
  полигонов трёх уровней. `M4.1.8` и `M4.4.6` закрыты (✅).
- `equipment_index` инициализирован `-1` (как `0x45bdd9`), `oracle_render.py`
  переключён на движковые дефолты `--uv texel` + пропуск полигонов без
  текстуры: `win2` без флагов побайтово равен `--uv texel`, а
  `--uv fit --keep-untextured` даёт исторический md5 `5010b6ab…`.
- Новый `docs/LAYOUT.md`: каталоги, канон по типам данных, таблица
  переменных окружения с читателями, кэш и инвалидация, маршрутизация
  «вопрос → инструмент». `NEXT.md` сокращён до очереди (история — здесь),
  `RE_AUDIT` §7 помечен как снимок аудита. Из `NEXT` убрана ссылка на
  `tools/loadbar.py` — такого файла в git не было.
- HUD: `gm/hud.py` (панель `PANEL` 320×78, ключ 0, `--self-test` 15/15,
  `--check-frame`), проверка в `regress` (`render / hud panel ncc`,
  114/114 quick). Измерение опровергло «2D-блит PANEL в (0,122)»:
  NCC +0.006…+0.035 в полном разрешении, по Sobel ≤ 0.08 при переборе
  сдвигов/масштабов, а стойки полосы различаются между `win2`/`win3`
  (`mean|d|` 21.6/24.5) — полосу рисует не этот блит (`docs/HUD_AUDIT.md`
  §2.4). Координаты текста HUD при этом остаются верными.
- Замечено: в том же дереве работала вторая сессия (коммиты `21aba34`,
  `d3218e5`, `ae85c96`); правило «один писатель на общий файл» записано в
  `docs/LAYOUT.md` §6.

## Ход 2026-10-23 (партии 4–8: RE-модели, ВМ, паритет)

- **Партии сабагентов (ветки → master, все worktree удалены).** 4:
  `feat/metric` (метрика кадра), `feat/equip` (`M6.2.4`), `feat/hudcompose`
  (`M4.4.5`). 5: `feat/sectorstuff` (`M5.2.7`, опкод 12 в C++ исправлен),
  `feat/effects` (`M4.2.6`), `feat/hudicons`. 6: `feat/economy` (`M6.2.6`:
  деньги `+0x332c`, `+0x3330` = salvage), `feat/screeneffects` (`M4.2.8`),
  `feat/campaign` (`M9.1`: счётчик `0x80f9cc`, 66 переходов, 24 миссии).
  7: `feat/dm` (`M9.2`: режимы 0/1/2/3, счёт `+0x3270`, HUD `0x492fdd`),
  `feat/screens` (`M8.2`/`M8.7`: 33 экрана, 34 перехода, слоты сейвов),
  `feat/pickup` (`M6.2.6`: триггер `0x469770`, регенерация, вирус, щиты).
  8: `feat/soundnames` (`M7.6`), `feat/uisound` (`M8.6`).
- **Исправления по ходу.** `SCAVENGERHUNT_GAME` в ВМ был `game_mode == 5`,
  в оригинале `== 3` (C++ и Python); `_h_generate` в Python (словарь =
  последняя строка) — трассы C++/Python совпали 36/36, 328 524 события
  (`M5.3.2`); `ITEM`-запись в `RECORDS.md`/`records.py` (`state -1` =
  фантом, `+0x54` = таймер перезарядки, `+0x5c` = слагаемое источника).
- **`sounds.sub` в v1.36 не загружается** (`0x40b510` ждёт `%ld`-счётчик,
  файл — пары), поэтому «95 отсутствующих имён» — это имена из первой
  колонки таблицы алиасов; реально 528/528 имён таблиц разрешаются в
  `SOU.IDX`. В C++ добавлен `res/sound_catalog.*` (CTest `sound_catalog`:
  5800 слотов, 528 ключей, 0 ненайденных, промах → `nullopt`).
- **Фикстуры паритета (`M3.2.7`).** `tools/re/export_parity.py` пишет
  ожидаемые значения Python-парсеров, `engine/tests/test_parity.cpp`
  проверяет их C++-парсерами без Python: 208 проверок, 1029 значений,
  0 расхождений (`sbw`/`sbt`/`sbc`/`sbm` из `SBM.IDX`/`sob`).
- **Рендер.** `render/postfx.*` — белый фейд `SET_CAMERA_HAZE`
  (`alpha = ftol(255·haze)`), `RenderOptions::haze`, CLI `--haze 0..1`
  (CTest `postfx`; на `level0` 160×120: `--haze 1` → все 255, `0.5` →
  минимум 127, среднее 169.02). `M4.2.4` → 🟡: LUT-математика доказана, но
  в 3dfx-сборке её не читает никто (один Glide-стейт), `--no-materials`
  не меняет метрику `win2`/`win3`.
- **ВМ.** `VmState` держит музыкальные банки (`LOAD/PLAY/STOP_UNLOAD`,
  memorize/replay) и тексты оверлея (`ADD_[SOURCE]SCREEN_TEXT`), событие в
  трассе осталось одно на команду (CTest `vm_music`). `M6.3.5` закрыт
  отрицательно: ботов в оригинале нет.
- **Проверено:** `ctest` 89/90 (только внешний `controls`), `regress --full
  --mirror` 123/124 (только `install_audit`), `vm_trace` 36/36, ROADMAP
  92✅ / 89🟡 / 52⬜.

## Ход 2026-10-23 (после партии 8: звук, ВМ-состояние, CLI)

- **3D-звук (`M7.3`).** `docs/AUDIO_3D.md` + `audio_3d.py` (61): расстояние
  `max+0.25(mid+min)` (`0x4344df`), гейн `trunc(volume−atten·2^n·dist)>>2n`,
  панорама по базису слушателя (`0x437909`), 10 409 3D-вызовов в корпусе.
  В C++ формулы вынесены в `audio/engine_3d.*` (CTest `engine_3d`, пять
  векторов из `audio_3d.py --calc`); микшер пока не переведён. Поправки
  внесены в `SOUND_FIELDS.md` (уточнения формул) и `SBT_RECORDS.md`
  (6-я колонка `SOUND_SOURCES` — `atten`, а не `pitch`).
- **Имена сэмплов в движке.** `gunmetal --sample NAME` (`res/sound_catalog.*`):
  `alarmk` → `archive=sounds name=ALARMK offset=24369 size=13619`, отсечение
  каталога `C:\Sounds\alarmk` работает, `DM1A01` уходит в `music`, промах —
  «not found (the original plays silence)», rc=0. CTest `cli_sample`,
  `cli_sample_miss`.
- **ВМ ведёт состояние.** `mission_counter` (`ds:0x80f9cc`, `NEXT_LEVEL` /
  `NEXT_LEVEL_NUMBER`, CTest `vm_campaign`), музыкальные банки и тексты
  (`vm_music`), запросы полных экранов `SHOW_*`/`REFRESH_SCREEN`
  (`vm_screens`). Все обработчики сохраняют ровно одно событие — `vm_trace`
  (C++ ↔ Python) не сдвинулся. Python-модель повторяет поля.
  `docs/VM_CPP.md` §4.4 — таблица полей с адресами.
- **`controls` снова зелёный.** `KEYCFG/*.KEY` — пользовательские данные:
  точные значения поставочных профилей проверяются только если файлы
  совпадают с манифестом 1.36 (CMake считает sha256 и ставит
  `GM_KEYCFG_SHIPPED=1`), иначе тест проверяет инварианты парсера. На
  изменённой установке: `ctest` 97/97.
- **Статусы.** `M6.5.4` → 🟡 (модель дверей и корпусные пины 1779/3964/45),
  `M8.5` — fade в обоих рендерах, переходы не подключены.
- **В работе:** `feat/chassis` (`M6.1.4`/`M6.5.3`), `feat/damage`
  (`M6.5.2`), `feat/gameover` (`M9.5`), `feat/audit` (независимая проверка
  чисел в свежих доках), `feat/scriptasm` (ассемблер `.sbt`, `M11.1`).

## Ход 2026-10-23 (партия 9: урон, game over, независимый аудит)

- **Урон (`M6.5.2` → 🟡).** `docs/DAMAGE.md` + `tools/re/damage.py` (63/63,
  33 байтовых якоря): `0x453b50` (плита → броня, перелив, `flags` 1/2/4),
  смерть `0x4536e0`, взрыв `0x477280` (`damage*(radius−dist)/radius`),
  цепной `0x4698b0`, вещи `0x470500`. Исправлено: `kHitscanMinDepth` = **0.0**
  (`[0x4a2658]` = 8 нулей, а не 0.32), ярлыки `+0x14c`/`+0x150` —
  `shield_max`/`armor_max` (`LIGHT` 8000/10000); `+0x3224` — таймер вспышки.
- **Game over (`M9.5` → 🟡).** `docs/GAME_OVER.md` + `gameover.py` (120/120):
  состояния «game over» нет, смерть → 136 тиков (1.49 с при 91 Гц) → респавн;
  `RESTART GAME` по Activate; титры `CREDITS.SML` — один раз на титуле
  (`0x487d94`, `pager_state` 1→2), пункт меню 3 грузит `LEVEL0A.SML`
  (поздравление), авто-титров после `level24` нет; рестарт из штаба
  `0x4428f0(1)`; экран конца уровня — опкод 91.
- **Независимый аудит (`docs/AUDIT_NUMBERS.md`).** `tools/qa/doc_numbers.py`
  перепроверил 38 чисел в 7 свежих доках своими инструментами: 36
  подтверждено, 2 расхождения исправлены — 35 переходов в таблице
  `UI_SCREENS` (34 записи JSON), **651** файл `MUSIC/*.sog` (598 строчных +
  53 `.SOG`). Уточнено: 11 513 — это item-инструкции, листовых — 119 971.
- **Пользовательские данные.** `KEYCFG/*.KEY` переписывает игра при
  переназначении клавиш, поэтому `controls` пинит точные значения только
  при совпадении sha256 с манифестом 1.36 (CMake → `GM_KEYCFG_SHIPPED=1`),
  а `install_audit` считает их `volatile`, как `GUNMETAL.CFG`. Итог:
  `ctest` 98/98, `regress --full --mirror` **124/124**.
- **ASan/UBSan.** `cmake --preset asan`: `engine_3d`, `vm_screens`, `parity`,
  `sound_catalog`, `postfx` — чисто (5 `__asan_report`, libasan линкуется).

## Ход 2026-10-23 (партия 10: ассемблер скриптов, цели/секреты, ASan, парность SML)

- **Ассемблер `.sbt` (`M11.1` → 🟡).** `tools/editor/sbtasm.py` +
  `docs/SBT_ASM.md`: обратный путь к `gm.sbtvm.decode` по сигнатурам
  `COMMANDS`/`CONDITIONS`/`TABLE_SIGS`; round-trip по 36 уровням —
  **15 061/15 061 блока побайтово** (3 542 023 байта), 147 777 инструкций,
  171 опкод, 43 условия; `--self-test` 11/11; CTest `sbtasm_selftest`,
  `sbtasm_roundtrip`. `render_exact` фиксирует места, где эталонный
  `render` теряет данные (`%g` у 2 478 блоков, 17 `ADD_LIFT`, 42 блока с
  вложенным `else`-кодом).
- **Цели/секреты (`M6.4.5` → 🟡).** `docs/OBJECTIVES.md` +
  `tools/re/objectives.py` (120/120, 43 якоря): 103 → `0x80d7ec`+`0x80d800`,
  104 — единственный писатель маски `0x80f9d0` (`0x45be4e`), условие 7,
  сброс `0x456f50`; tally секретов `player+0x33cc` (`0x4536b0`, сброс
  `0x45280f`); корпус — 25 уровней/35 объявлений, 51 первичная строка
  (всех 67, разных 58), 19 уровней ставят биты, 101 секрет / 105
  инкрементов, 38 сайтов экрана конца уровня. Поправки в `CAMPAIGN_FLOW`
  §4 и `LEVEL_LOAD` §4.
- **ASan/UBSan по всему набору (`M12.1` → 🟡).** `build/asan`:
  **101/101** за 416 с; найден и исправлен `stack-use-after-return` в
  `test_ai` (`Ai::attach` + перемещённый `Things`). Подробности —
  `docs/SANITIZERS.md`.
- **C++-урон сверен с доком.** CTest `damage_model`: 45/12/3 удара до
  смерти, перелив плиты в броню, `flags` 1/2/4, `mult_plate`/`mult_hull`,
  спад взрыва — всё совпало с `damage.py --calc`.
- **Парность SML.** `export_parity.py` + `test_parity.cpp`: 40 файлов
  `SML/*.sml|*.sme` (chars/tags/FNV текста), итог **248 проверок, 1149
  значений, 0 расхождений**. Фикстуры сразу поймали разницу: `decode_auto`
  — вход `.sme`, для plain `.sml` он даёт сдвинутый текст при том же числе
  тегов.
- **Аудит расширен** (`doc_numbers.py`): +`GAME_OVER` (38 сайтов опкода 91,
  0 строк `GAME OVER`) и +`SBT_ASM` (блоки/байты/инструкции/опкоды) —
  **41/41**, 9 доков; `AUDIT_NUMBERS.md` отмечает исправленные 35
  переходов и 651 `.sog`.
- **Снимки обновлены:** `CURRENT_STATE.md` (943 коммита, CTest 103/103,
  regress 124/124, этапы 62 %) и `docs/RE_AUDIT.md`.
- **В работе:** `feat/chassis` (`M6.1.4`/`M6.5.3`), `feat/water` (`M7.7`),
  `feat/sbtwrite` (`M11.4`, писатель контейнера `.sbt`).

## Ход 2026-10-23 (партия 11: подводное состояние, писатель `.sbt`, перф-снимок)

- **Подводное (`M7.7` → ✅, фильтра нет).** `docs/UNDERWATER.md` +
  `tools/re/underwater.py` (124/124, 42 якоря): флаг `player+0x3200` бит
  `0x8` (ставит `0x444fd2`, снимает `0x444f74` по типу сектора `+0x323c` →
  `0x42d99f == 1`); экран — подмена палитры `FRONTEND/WATERPAL.BMP`
  (`0x83d2c4` → `0x4847b0`), фейда нет; физика `0.75`/`0.5` и вертикаль
  `−2.4`; **DSP-фильтра нет** (20 двордов голоса без фильтра, микшер
  `0x409330`, ни одно из 9 чтений игрока в звуке не тестирует бит `0x8`).
  Поправки: `+0x323c` — `sector`, а не `model_id`; условия 47/48/49 —
  `value`; 36 сайтов `SET_CAMERA_HAZE` под `SOURCE_UNDERWATER` — блок
  смерти/взрыва, а не вход в воду.
- **Писатель `.sbt` (`M11.5` → 🟡, `M11.1` → ✅).** `tools/editor/sbtwrite.py`
  + `docs/SBT_WRITE.md`: round-trip **36/36 побайтово** (6 793 572 Б),
  замена подскрипта с пересчётом `SCRIPT.m` и смещений колонки 2
  (`--verify-replace`: +1 байт сдвигает 257…364 смещения, прочие записи
  идентичны), `--check`/`--expect`, 18 self-тестов; CTest
  `sbtwrite_selftest`/`sbtwrite_roundtrip`/`sbtwrite_replace`.
- **Перф-снимок (`M12.3` → 🟡).** `tools/qa/perf.py` + `docs/PERFORMANCE.md`:
  разбор 8.5/31.9/47.8 мс (level0/dm1/level24), C++ загрузка 61.7 мс,
  ВМ 1.6 мс, холодный рендер 182/243 мс (320×200/640×400), SOU1 54 408
  кадров за 4.9 мс, RSS 52 МБ; на `/mnt/c` всё вдвое дороже (9p).
  60 FPS не измерен — нужен прогретый цикл.
- **Аудит:** `doc_numbers.py` расширен `OBJECTIVES` (25/35, 51, 25/101/105) —
  теперь **44/44** по 10 докам.
- **«Укажите установку» (`M12.5` → 🟡).** Неверный `--game` теперь даёт
  `[error] game: directory not found: <путь>` и подсказку (`-g/--game`,
  `GUNMETAL_DIR`, `gundx.exe` в каталоге), rc=2; CTest `cli_no_game` (без
  установки). Лаунчера/упаковки по-прежнему нет.
- **Гейты:** `ctest` **110/110**, `regress --mirror` 114/114,
  `regress --full --mirror` **124/124** (75,5 с).

## Ход 2026-10-23 (партия 12: упаковка BIGFILE)

- **`M11.6` → ✅.** `tools/editor/bigpack.py` + `docs/BIGFILE_WRITE.md`:
  round-trip всех 5 поставочных пар `*.IDX`+`*.DAT` — **побайтово**
  (6061 запись, 151 737 971 Б, ~2 с); замена записи `57CHEV` в
  `PICS/SBM.IDX` (тот же размер / +1360 / −7393): остальные 1647 записей
  идентичны, смещения сдвигаются одинаково, оракул и `gm.py list` видят
  1648 записей; `--check` сверяет таблицу/цель/остальные; отказы — запись
  в каталог игры, существующий `--out`, имя вне `[A-Z0-9_+.-']`
  (игра ищет ключ в верхнем регистре, `0x40d1e2`). Формат с адресами:
  заголовок 12 Б, запись 12 Б, порции `0x400`, `offset` → `fseek`
  (`0x40d4db..0x40d819`). 15 self-тестов; CTest `bigpack_selftest`,
  `bigpack_roundtrip`.
- **Сквозная проверка писателя `.sbt`:** правленый `level0.sbt`
  (`SET_CAMERA_HAZE 1.0 → 0.5`, блок `0x6f92`) загружается **C++-движком**
  с той же структурой (8 записей, 350 подскриптов, 62 497 Б кода).
- **Документация (`M11.8` ✅, `M12.6` ✅).** `docs/PLAYER_GUIDE.md`,
  `docs/MODDING.md`, `docs/FAQ.md`: все команды запущены (таблица
  «команда → rc → первая строка»), пример мода воспроизведён (sha256
  `9ffa3b89…`), битых ссылок нет.
- **Парность SOU.** +40 заголовков `sounds/SOU.IDX` — **288 проверок,
  1309 значений, 0 расхождений**; сравнивается объявленная длина, а не
  чётная обрезка кодека (у 25 записей длина нечётная).
- **Гейты:** `ctest` **112/112**, `regress --mirror` 114/114,
  `regress --full --mirror` 124/124.

## Ход 2026-10-23 (партия 13: фаззинг остальных парсеров, 64-bit, пути архивов)

- **Фаззинг (`M6.4` → ✅).** В `tools/fuzz_formats.py` добавлены `sou`
  (SOU0/1/2-заголовок), `sou2` (запись `MUSIC/SOU.IDX`), `sml`
  (`decode_auto` + теги) и `sog`; уже были `.soo`, `.soe`, `.soa`,
  `sou1`-декод. Полная матрица — **1619 кейсов, 0 падений** (2 с),
  быстрая (`--quick`, в `regress.py`) — 723.
- **64-bit (`M12.2` → 🟡).** Сборка Zig по умолчанию — **ELF 64-bit
  x86-64** (`file build/sandbox/engine/gunmetal`), весь набор 112/112 и
  ASan 101/101; отдельной 32-битной цели нет. Не хватает Windows-сборки и
  CI-матрицы.
- **`M11.6` + `bigpack`:** относительные имена архивов (`PICS/SBM.IDX`)
  теперь разрешаются под `--game`, как в движке (16/16 self-тестов);
  добавлен `tools/editor/README.md` с проверенным циклом правки.
- **Снимок:** `CURRENT_STATE.md` (988 коммитов, 112/112 CTest, этапы
  95 ✅ / 97 🟡 / 28 ⬜ / 1 ⛔).

## Ход 2026-10-23 (партия 14: релизный чек-лист, DoD, regression модов)

- **`M12.8` → 🟡.** `docs/RELEASE.md`: предусловия, гейты (`ctest`
  114/114, `regress --full` 124/124, ASan 101/101, `install_audit` ok,
  фаззинг), состав релиза без ассетов, версия (`config.h::kVersion` =
  `0.1.0`) и порядок тега. Тег не ставится: нет игрового цикла/фронтенда,
  автопрохождения, 60 FPS, Windows/CI, DM с ботами; `docs/avatar.png` —
  производный рендер модели, убрать/заменить (`M12.7`).
- **`M11.7` → 🟡.** `tools/qa/mod_regress.py` (CTest `mod_regress`):
  disasm блока `0x6f92` → правка `SET_CAMERA_HAZE` → asm → `sbtwrite` →
  `gunmetal -l level0 --info`; структура до/после совпадает, правка видна
  оракулу. В оригинале (`gundx.exe`) мод не запускался — ручной шаг.
- **`M6.3.6` → 🟡.** Баланс из данных: `DEFINE_WEAPONS`/`DEFINE_AMMO` →
  `Weapons`, `DEFINE_EQUIPMENT` → `EquipmentTable`, `DEFINE_PLAYER_TYPES`
  → параметры шасси. Осталось: «гипотезный слой» в `weapons.h` и нет
  эталонных трасс баланса.
- **DoD.** «Тихие пропуски» → 🟡: 8 CLI-путей с отсутствующим ресурсом
  дают `[error] …` и rc=2 (CTest `cli_no_game`, `cli_missing_dump`);
  «редактор/компилятор/импорт-экспорт» → 🟡 (компилятор ✅, редактора
  уровней нет); «нет ассетов» → 🟡 (`docs/avatar.png` — производный
  рендер, к релизу убрать).
- **Аудит:** `doc_numbers.py` + `BIGFILE_WRITE` (5 архивов/6061
  запись/151 737 971 Б) — **45/45 по 11 докам**.
- **Гейты:** `ctest` **114/114**, `regress --mirror` 114/114.

## Ход 2026-10-23 (партия 15: редактор записей сцены)

- **`M11.3` → 🟡.** `tools/editor/sceneedit.py` + `docs/SCENE_EDIT.md`:
  записи сцены `.sbt` (всё, кроме `SCRIPT`) — `--list`, `--get`,
  `--set NAME[INDEX]=VALUE`, `--replace-record`, `--check`/`--against`,
  `--fields` с provenance (док + VA). Round-trip **36/36** (6 793 572 Б,
  241 запись сцены), self-test 38/38. Реальная правка:
  `THING_TYPES.speed[25]` 50 → 55 в `level0.sbt` — остальные записи
  побайтово те же, оракул читает 55, **движок грузит** с той же структурой;
  замена записи с ростом +20 Б (таблицы смещений нет) тоже грузится.
  CTest `sceneedit_selftest`, `sceneedit_roundtrip`.
- **DoD/уровень:** неверное имя уровня теперь говорит, что нет и `.sbw`,
  и `.sbt` (`wrong level name?`), CTest `cli_missing_level`.
- **Гейты:** `ctest` **118/118**, `regress --full` **124/124**.

## Ход 2026-10-23 (партия 16: редактор геометрии `.sbw`)

- **`M11.2` → 🟡.** `tools/editor/sbwedit.py` + `docs/SBW_EDIT.md`: шесть
  документированных полей полигона (`texture/material/lightmap/brightness/
  overlay/flags`), `--list/--get/--set/--check/--audit/--fields`; дисковые
  смещения ≠ движковым (`material` на диске `+0x04`, `lightmap` `+0x02`,
  `flags` после списка индексов). Round-trip **36/36** (28 192 объекта,
  235 069 полигонов, 40 037 031 Б), self-test 59/59, `--audit` диапазонов
  поставки. Реальная правка `texture 1 → 28` (`WHITE6`): 1 байт, оракул
  видит 28, движок грузит и **рендерит** (75 из 64 000 пикселей отличаются).
  CTest `sbwedit_selftest`/`sbwedit_roundtrip`/`sbwedit_audit`.
- **Аудит:** `doc_numbers.py` + `SBW_EDIT` — **48/48 по 14 докам**.
- **Гейты:** `ctest` **121/121**, `regress --full --mirror` **124/124**.

## Ход 2026-10-23 (партия 17: шасси — выбор, 57 CHEVY, «8»)

- **`M6.1.4` → 🟡, `M6.5.3` → 🟡.** `docs/CHASSIS.md` +
  `tools/re/chassis.py` (23/23): отдельного меню нет — выбор = покупка в
  штабе (опкод 191 → `0x47b990`, курсор `+0x3460`, цена `0x479dd0` →
  `0x47a980`: тип `+0x3464`, цена `+0x3468`, девять полей
  `0x47ac1a…0x47af3e`). Модели `DEVSTTR/TICK/SPARTAN/STEALTH` + 4 обломка;
  звук `thuml/thumm/thumh/thumoil`; HUD `Film/tank.fdx` 8 строк.
- **57 CHEVY** — ветвь `IF GOD_MODE` (`[0x62cad4]`, пишет только чит `/god`
  `0x44a9e8`); в DM/SCAV её нет. «8 шасси» числом не подтверждается:
  играбельных **4** в кампании, **5** с `/god`; «8» — модели (4+4),
  строки витрины и 8 слотов имён (по числу игроков).
- **Исправлено в `docs/PLAYER_FIELDS.md`:** `+0x33e0` — индекс сэмпла
  двигателя (не иконка), `+0x3460` — курсор магазина (не «запрошенный
  тип»); `+0x3464` — текущий тип.
- CTest `re_chassis`; гейты `ctest` **122/122**, `regress --mirror` 114/114.

## Ход 2026-10-23 (партия 18: движок читает девять полей шасси)

- **`M6.1.4` (движок).** `game/chassis_loadout.*` — единственное место, где
  названы девять применяемых полей `DEFINE_PLAYER_TYPES`: `chassis_spec`
  (броня ×100, модель 0..255, звук `-1`/`0xFFFFFFFF` → −1, отказы на
  NaN/отрицательных/дробных), `chassis_motion` (op0/op1/op4 →
  `ChassisMotion`, до этого в контроллере были зашиты числа LIGHT),
  `shop_price` (`0x479dd0`: LIGHT 26 000 … CHEVY 560 000) и
  `chassis_from_level` (обход IF/ELSE; в файле лежат все ветки, включая
  `IF GOD_MODE`). CTest `chassis_loadout`: пять шасси из
  `docs/CHASSIS.md` §4, реальные `level0`/`dm1`, негативы.
- `build/warn`: новые файлы — 0 предупреждений.
- **Гейты:** `ctest` **123/123**, `regress --mirror` 114/114.

## Ход 2026-10-23 (партия 19: безголовый прогон `--sim`)

- **`M6.5.1` → 🟡.** `gunmetal --sim LEVEL --ticks N` (`docs/SIM.md`,
  CTest `sim_check`): `load_level` → `vm_state_from_level` →
  `GameRuntime::build` → глаз в первом спавне → мост `ai_apply_aim` →
  `tick(N)`. `dm1` 500 тиков: 141 вещь, 61 дверь, 129 переключателей,
  5 агентов, 19 860 шагов автоматов / 14 952 сдвинулось. Дайджест
  (FNV-1a по вещам, автоматам движения и дверям) совпадает при повторном
  прогоне: `tools/qa/sim_check.py` запускает `--sim` дважды.
  Найдена и исправлена реальная ловушка: float→unsigned при отрицательных
  координатах ронял процесс (`panic: outside the range`), теперь через
  `int64`.
  Нет игрока/ввода (нужен игровой цикл `M6.5`/`M8`).
- **Гейты:** `ctest` **124/124**, `regress --full --mirror` **124/124**.

## Ход 2026-10-23 (полоса кокпита: от «не PANEL» до точного воспроизведения)

- Трассировка записи в живом процессе (`tools/win/trace_band.lua`, CE): экран
  800×600, строки 366/483/592, стек `0x4924F5` ← `0x44821D` → владелец
  HUD-слой `0x491a70`, блит `FUN_00a1b510` (`0x491f10..0x491f2d`) из буфера
  `0x678188` через LUT `0x682f90`.
- Дампы (`tools/win/dump_lut.lua`): `lut[10][panel]` совпал с экраном движка
  на 77.5 %; остальное — текст/иконки (16.5 % ярче).
- Загрузчик `0x47efe0` разобран: палитра (`0x400`, каналы `>>2`), проверка
  320×78, строки копируются **в файловом порядке** — перекраски нет,
  `flipV(ассет)` == дамп буфера на 100 %. Добавлен `gm/hud.py:engine_order()`.
- `oracle_render.py --hud` (`--hud-lut/--hud-level/--hud-top`): `frame_metric`
  `win2` frame 0.183→0.228, hud 0.000→0.187; `win3` frame 0.139→0.286,
  hud 0.078→0.318. `regress render,ui` 18/18.

## Ход 2026-10-23 (текстовый слой HUD: 100 % совпадение)

- `0x47faf0` разобран: `dest = buf + pitch*y + x`, строки глифа идут вниз,
  индекс 0 прозрачный; ячейка занимает `y-cell_h … y-1`.
- `0x491a70+0x133` вызывает `0x4926a0` уже по экрану **после** блита панели,
  поэтому текст/иконки/заливки **не фейдятся** LUT (на экране `92`, в LUT
  `LUT[92]=91` — это и было «16.5 % ярче»).
- Из декомпиляции взяты константы: шрифт 6 `FONTS3` для подписей, 10 —
  патроны/экипировка, 11 — убийства, 9 — урон; `x` правого края 72/156/246
  с шагом 8, `y` = константа + 6.
- Проверка по `gm_screen.bin`: **965/965** пикселей текста совпали точно,
  полоса 77.5→81.8 % индексов.
- `gm/hud.py`: `Font`/`load_font`/`draw_text`/`hud_text`/`HUD_LABELS`,
  `compose(text=, fonts=)`, `--text`, `--state`; self-test 40/40.
- `oracle_render.py --hud` рисует текст (`--no-hud-text`, `--hud-state`):
  `win2` hud 0.187→0.409 (frame 0.228→0.304), `win3` hud 0.318→0.501
  (frame 0.286→0.354); `regress render,ui` 18/18.
- Иконки того же слоя: `WICON1` в `(23,162)` без ключа и `EQUIP12`
  (= зеркальный `EQUIP0`) в `(201,162)` — 576/576 пикселей каждая. Полоса
  по индексам 81.8→**86.9 %**; `win2` hud **0.583** (frame 0.351), `win3`
  hud **0.737** (frame 0.406). `--self-test` 44/44.
- Остаток 13.1 % — верхние 15 строк полосы (2623 из 2929 px); строки 15…69
  совпадают на 98.3 %. Верхняя полоса не сдвигается (лучший dx=dy=0 даёт
  45 %), значит там другой растр: 25-строчный блит `&DAT_0065e138`
  (`0x491a70+0x4b`) берёт буфер, заполняемый в рантайме (в exe нули).
- Шкалы заряда: 4 полосы (17 px макс, цвет `0xf3`/`0x2b`/`0xff` по длине) из
  `0x492f..`, строки из кода −8, вертикальные растут вверх. По дампу
  **60/60** пикселей; `hud_gauges()`, self-test 53/53. `win2` hud 0.583→0.627,
  `win3` 0.737→0.777.
- Верхние 15 строк полосы — 3D-кокпит: индекса 35 (полосы 107,75,19) в `PANEL`
  нет вовсе, поиск по всем 320-широким ассетам даёт максимум 23.6 %.
- Шкала тяги/патронов: геометрия из кода — тяга 4 строки (163…166) × ≤53 px
  с (202,163), патроны 2 строки (182…183) с (50,182), цвет `0xf3`. Число
  строк тяги подтверждено косвенно: подпись `THROTTLE` = `puVar9-7` = 159,
  а 159 измерено на 131/131 пикселе. Длины не проверены — в дампах нули.
  `hud_bars()`, self-test 57/57.
- Силуэт машины в гексагоне: `RPVICONS` 60×29, три варианта по 20 px; код
  берёт 19 строк с шага 0x3c из `0x6601a4 + stage*0x14`, ключ `0x73`; буфер
  снизу вверх → срез переворачивается; (147,155), строки 5…23. 135/135.
- Тяга в живом дампе ненулевая: 26 px × 4 строки (155…158), заливка до
  подписи — 104/104. Вертикальные полосы заряда перемерены: 17, не 13.
- Итог полосы: 88.0 % по индексам; строки 15…69 — 99.6 %; `win2` hud 0.635
  (frame 0.393), `win3` 0.795 (0.432). self-test 61/61.
- Остаток строк 15…69 полосы локализован: всё, кроме строки 145 (34 px,
  x 13…31), совпадает **точно** (0 px). Строка 145 — яркая LED-полоса
  (147/145/144/201/192/146), то есть анимация индикаторов; строки 130…144 —
  3D-кокпит.
- Уровень затемнения полосы в `win2`/`win3` разный и в sidecar не пишется
  (`state.ps1` не дампит `player+0x81c24c`). Подбор по `hud`-score: `win2`
  k=4, `win3` k=7 (граница клампа при `DAT_006e89c0`). С `--pitch-offset 12`
  это даёт `win2` hud 0.829 / frame 0.470, `win3` hud 0.833 / frame 0.459
  (было 0.635/0.393 и 0.795/0.432). В `frame`-score оптимум сместился на +12°
  для обоих кадров; `docs/FRAME_METRIC.md` помечен как исторический.
- `tools/re/hud_level.py`: восстановление уровня фейда полосы из снимка
  (средняя |Δ| по RGB для 16 таблиц, с исключением текстового слоя и
  3D-строк). `win2` → 4, `win3` → 7 — совпало с выбором метрики; self-test
  4/4, вошёл в `regress.py` (`render,ui` теперь 19/19).
- Третий снимок `win1`: `hud_level.py` даёт 11 (таблица почти тождественная),
  выравнивание полосы 0/0, средняя |Δ| 53.0 — как у `win2` (50.5). Низкий
  общий `hud`-score (0.692) объясняется 3D-кокпитом в верхних строках
  (`world` у `win1` = 0.0, камера sidecar'а не совпадает), а не 2D-слоем.

## Ход 2026-10-23 (партия 20: отладчик скриптов, корпусный `--sim`)

- **`M11.4` → 🟡.** `tools/editor/sbtdbg.py` + `docs/SCRIPT_DEBUG.md`
  (self-test 26/26): `--run` с трассой и эффектами шага (`player.cash
  0 -> 20000`, `vm.ticks`), стопы `--break-offset`/`--break-op`/
  `--break-event` (до инструкции) + `--continue`, `--watch` изменений полей,
  `--list-blocks` (350 блоков / 62 497 Б на `level0`), CFG
  `--graph --dot/--mermaid` (`level0 0x6f92` — 84 узла / 92 ребра,
  `dm1 0x2a8d` — 9 / 10 / 1 цикл). Все 36 уровней — 0 падений.
  CTest `sbtdbg_selftest`/`sbtdbg_run`/`sbtdbg_break`.
- **`--sim` на всём корпусе:** 36/36 уровней на 100 тиках (0 падений,
  ~40 с) и на 2 000 тиках (≈22 с игры, 0 падений, 1 310 с); CTest
  `sim_corpus` (full) — 36 уровней, 0 failed.
- **Гейты:** `ctest` **128/128**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 21: игрок под управлением, лайтмапы)

- **`M6.5.1` → 🟡 (игрок).** `engine/src/game/sim_player.*` (CTest
  `sim_player`) + CLI `--sim-drive PLAN` (`docs/SIM_PLAYER.md`,
  CTest `cli_sim_drive`, `sim_drive_check`): `PlayerState`/`step` и шов
  оружия, без новой физики. `dm1`: `forward=1:900` — 1508.75 юнита,
  `fire:300` — 18 выстрелов MACHINE GUN (патроны 5000 → 4982), повтор
  бит-в-бит; дайджест включает игрока.
  **Найдено:** пол под спавном на реальных уровнях вывернут
  (`front_face == false`) → `on_ground = 0` при посадке капсулы;
  расхождение с `docs/PLAYER_CONTROLLER.md` §5.2 записано в `docs/SIM.md`.
- **`M11.2` (лайтмапы).** `tools/editor/lightmapedit.py` +
  `docs/LIGHTMAP_EDIT.md`: `--list/--export/--import/--fill/--scale/--check`,
  round-trip **51/51** (67 200 записей, 24 214 080 Б); `--fill` записи 0 у
  `level0` — 128 байт, оракул видит 128, движок грузит те же
  405 записей/51 792 текселя, рендер отличается на 88 пикселей из 64 000.
  Попутно `docs/FORMATS.md` §4: `.sbc` — **36 в корне + 15 в `SOBS/`**
  (было «34 + 17»).
- **Аудит:** + `SCRIPT_DEBUG`, `LIGHTMAP_EDIT` — **50/50 по 16 докам**.
- **Гейты:** `ctest` **133/133**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 22: пол держит, снаряд убивает, объекты `.sbw`)

- **КРИТИЧНО, исправлено: игрок проваливался сквозь пол.** У поставочных
  полигонов обход индексов противоположен сохранённому `SbwPoly::normal`,
  поэтому полы классифицировались как потолки. Модель: `Triangle::normal`
  (обход — рендер) + `data_normal` (файл — классификация), коллизия
  двусторонняя; `CollideFilter` выбирает пол/стену/потолок по сохранённой
  нормали. Наивный «перевернуть обход» проверен и откачен (стены перестают
  держать). Итог: `on_ground=1` на `dm1`/`level0`/`level1`, стены держат
  (`wall_ticks` 92/243), `level0` floors 558 / walls 2082 / ceilings 522.
  CTest `collide`, `player_controller`, `sim_player` обновлены с обоснованием.
- **`M6.5.1` (снаряд и урон).** `SimPlayer::set_projectile` +
  `docs/SIM_PROJECTILE.md` (CTest `sim_projectile`): ROCKET (слот 7) в `dm1`,
  снаряд TT31, взрыв о стену, а в тесте — OILDRUM 1000 HP/1000 щитов →
  `hp=0`, `alive=false`; CLI `--sim-projectile` (CTest `cli_sim_projectile`):
  4 снаряда / 2 взрыва за 300 тиков.
- **`M11.2` (объекты `.sbw`).** `tools/editor/sbwobj.py` +
  `docs/SBW_OBJECTS.md`: round-trip 36/36 (28 192 объекта, 357 818 вершин),
  сдвиг `origin` двигает объект целиком, рендер меняется.
- **Аудит:** `SBW_OBJECTS` — **51/51 по 17 докам**.
- **Гейты:** `ctest` **138/138** (quick 124 + full 14),
  `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 23: урон/смерть/респавн, кэш сэмплов)

- **`M6.5.1` срез 3.** `SimPlayer::set_vitals/respawn/deaths/damage_taken`
  (`docs/SIM_DAMAGE.md`, CTest `sim_damage`): виталы `LIGHT` (броня 10 000,
  щиты 8 000 на сторону), смерть по `armor <= 0` (`0x453b50 → 0x4536e0`,
  не `status_flags & 0x20`), самоурон ROCKET'ом 5 060 за попадание,
  смерть на 328 тике, респавн восстанавливает шкалы и точку.
  CLI: `--sim-vitals ARMOR,SHIELD`, `--sim-respawn N` (CTest
  `cli_sim_respawn`, `cli_sim_vitals_bad`): 3 смерти / 1 500 урона за
  600 тиков.
- **`M7.4` (кэш сэмплов).** `engine/src/audio/sample_cache.*` +
  `docs/SAMPLE_CACHE.md` (CTest `sample_cache`): имя → SOU0/1/2, refcount
  (`0x4cebd8`/`0x4cf1a8`), бюджет `0x100 + 2·samples`, LRU только для
  `refcount == 0`, статистика; `level0` — 300 слотов, 137 имён, все
  разрешаются; гипотезы (16 МиБ, LRU, стоимость) помечены в доке.
- **Аудит:** + `SAMPLE_CACHE` — **52/52 по 18 докам**.
- **Гейты:** `ctest` **142/142**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 24: схема форматов единым источником)

- **`M3.2.1` → 🟡.** `tools/qa/schema_check.py` + `docs/SCHEMA.md`: схема в
  одном месте, проверка трёх сторон — C++ структуры, Python-парсеры, данные
  (36 `.sbw`, 36 `.sbt`, 51 `.sbc`). Self-test 19/19, `--check --strict` —
  25 подтверждено, 0 расхождений; дрейф ловится (подмена `n_verts`,
  типа `radius`).
- **Найдено:** `n_idx` доходит до **26** (`level22.sbw` @0x3d18b), док
  обещал 12 — `docs/FORMATS.md` §10.1 исправлен; `gm/sbc.py` docstring —
  «36 + 15» вместо «34 + 17».
- CTest `schema_check_selftest`, `schema_check`.
- **ASan** на новых модулях (`sample_cache`, `sim_damage`, `sim_projectile`,
  `sim_player`): 6/6; корпусный `--sim` — 36 уровней, 0 падений.

## Ход 2026-10-23 (партия 25: минимальные фикстуры форматов)

- **`M3.2.7` → 🟡.** `tools/qa/fixtures.py` + `docs/FIXTURES.md` (CTest
  `fixtures_selftest`/`fixtures_write`/`fixtures_check`): 10 минимальных
  файлов (`.sbw` 365 Б, `.sbt` 4013 Б, `.sbc` 16 Б, `.sbm` 76 Б, `.sob`,
  `.soo`, `.sml`, BIGFILE), побайтовый `parse → to_bytes`, детерминизм,
  неизвестная запись — отдельный негативный файл. Движок читает те же байты:
  `--info` → vertices=4 polygons=1, records=3, SBC0 1 запись/4 текселя.
- **Найдено и исправлено (DoD):** `--dump` на битом `.sml` возвращал 0
  (резервная ветка без проверки). Теперь файл без разметки и без разбора —
  `[error] dump: not SML markup` и `rc=2`; поставочный несбалансированный
  `SML/prelevel/dm4.sml` (единственный из 149) остаётся с `[warn]`.
  Порча байта: 8 из 9 отвергнуты движком, девятый (`.sml`) — теперь тоже.
- **Гейты:** `ctest` **147/147**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 26: безголовый фронтенд-автомат)

- **`M8.2` → 🟡.** `engine/src/ui/frontend_menu.*` + `docs/FRONTEND.md`
  (CTest `frontend_menu`): 34 экрана §2, 35 строк §4 (36 записей, 35
  включённых; титры — выключенная строка), таблица переходов с адресами
  сайтов, `back()`, алиас `0x48e212`, снимки экранов 30/34/35 с подписями
  bindings из `0x4bc558`.
- BFS: по полному графу **34/34**, из `MAIN` — 18/34; 12 входных рёбер
  (пагер, видеорежим, мастер новой игры) вынесены в `entry_edges()` и
  `feed()` не применяются — расхождение описано в доке §2.2.
- **CLI:** `--menu`, `--menu-keys up,down,enter,esc,back,text:0xNN`,
  `--menu-screen ID`, `--menu-json` (CTest `cli_menu*`): `down,down,enter`
  → GRAPHICS OPTIONS, `esc` → QUIT GAME?, неизвестный ключ/экран — `[error]`
  и код 2. Игра для меню не нужна.
- **Гейты:** `ctest` **153/153**.

## Ход 2026-10-23 (партия 27: движущиеся коллайдеры дверей)

- **`M6.4.1` → 🟡 (модуль).** `engine/src/world/moving_collider.*` +
  `docs/MOVING_COLLIDERS.md` (CTest `moving_collider`): панель сдвигается на
  `Door::closed → Door::open` (цель лерпа `0x44dfc0`), нормали/`bounds`
  пересчитываются; `raycast`/`sphere_cast` **побитово** совпадают с
  `CollisionWorld` (128 лучей/128 сфер: max |dt| = 0, max |dn| = 0).
- Факты: у 1779/1779 дверей `object.origin == closed`; 1421 скользящая,
  105 диагональных, 113 движущихся распашных — последние честно отказывают.
- Закрытая дверь держит луч/сферу, открытая пропускает; лифт — `lift_offset`.
  Нет: интеграции в рантайм (в работе отдельным срезом), кабины лифта,
  объект-объект, broad-phase.
- **Гейты:** `ctest` **154/154** (quick 140/140).

## Ход 2026-10-23 (партия 28: runtime-пагинация SML)

- **`M8.1` → 🟡 (пагер).** `engine/src/ui/sml_pager.*` +
  `docs/SML_PAGER.md` (CTest `sml_pager`): курсор над страницами
  `text_layout` 267×167; `<p>` — не разрыв страницы (`+line_height/2` и
  перевод строки). 6 документов / 18 страниц / 230 строк: `help.sme` 3/42,
  `CREDITS.SML` 5/64, `brief/level22.sme` 5/74; границы без изменения
  состояния; поймана регрессия кэша атласов (шрифт 3 после шрифта 1).
- **CLI `--pager`:** `--page N`, `--pager-keys next,prev,first,last,line,up`,
  `--all`; `help.sme` → `page 1/3 pages=3 lines=42` с текстом и координатами
  строк (CTest `cli_pager`, `cli_pager_keys`, `cli_pager_bad`).
- **Гейты:** `ctest` **158/158** (quick 144/144).

## Ход 2026-10-23 (партия 29: двери реально держат игрока)

- **`M6.4.1` срез 2 — интеграция.** `CollisionWorld::set_dynamic`
  (динамические треугольники + подавление статики по `(sector, object)`,
  отказ на NaN/`1e12` с сохранением прежней позы) и
  `GameRuntime::sync_door_colliders()` (пересборка только при смене
  `open_fraction`); `docs/DOOR_COLLISION.md`, CTest `door_collision`.
- Числа: `level0` — 3162 статических / 422 динамических / 24 подавленных;
  `dm1` — 11781 / 764 / 38. Закрытая створка `dm1 sec=17 obj=37` держит
  игрока (`x=8010.000001`, `wall_ticks=121`), открытая пропускает всю
  толщину (`x=8005.000001`, дальше статическая стена `x=8002`).
- Паритет: пустая динамика → луч `t=40`, `index=6` побитово; `collide`,
  `player_controller`, `moving_collider`, `sim_player`, `thing_motion` —
  зелёные без правок пиннингов. Цена: +12% на `--sim level23 --ticks 100`
  (80 дверей); в покое — 80 сравнений `double`/тик.
- **Нет:** распашные (357, 113 движутся) статичны; кабина лифта без
  привязки; объект-объект; `overlap_aabb` только статика.
- **Гейты:** `ctest` **159/159**, `regress --full --mirror` **125/125**,
  `build/warn` — 0 предупреждений.

## Ход 2026-10-23 (партия 30: музыкальные огибающие)

- **`M7.5` → 🟡 (модель).** `engine/src/audio/music.*` + `docs/MUSIC.md`
  (CTest `music`): линейные `fade_in/fade_out` (`N = round(sec*rate)`,
  `gain(k)=k/N`), кроссфейд `(1-t)·old + t·new` без клиппинга
  (пик 0.9977, `old²+new² ≤ 1`), `finished()`, `mix_into` во float.
  Доказано из данных: входная громкость `0x84a68c` 0…32; музыкальный путь
  всегда 32 000 Гц.
- **CLI:** `--fade-in/--fade-out SEC` → огибающая применяется к PCM офлайн
  (CTest `cli_music_fade`): `Dm1A` — 8 треков, 22.583 с, gain
  first=0.032000, mid=1.000000, last=0.000000. Некорректное значение —
  `[error]`/`gunmetal: --fade-in wants ...` и код 2.
- **Гейты:** `ctest` **162/162**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 31: распашные двери)

- **`M6.4.1` срез 3.** `DoorAxis::Rotating` в `world/moving_collider.*`:
  поворот панели вокруг центра, нормали поворачиваются как векторы,
  `plane_offset`/`centroid`/`bounds` пересчитываются; рантайм больше не
  пропускает распашные (`skipped_rotating = 0`), CTest `swing_door`.
- Числа: при `f=0` поза **побитово** равна статике; на полуугле длины рёбер
  сохранены (max |dL| = 8.5e-14), плоскости через центр (9.6e-14),
  |угол − половина| = 5.1e-15; игрок в закрытую створку упирается
  (`wall_ticks=96`), в открытую проходит (`wall_ticks=0`, 233.75 юнита).
- Рантайм `level0`: 61 дверь / 61 панель / 23 распашных / 1038 динамических
  треугольников / 61 подавленный; `door_collision` обновлён (распашная
  теперь тоже подавляет статику).
- **Гейты:** `ctest` **163/163**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 32: событийный мост урона)

- **`M6.2.5` → 🟡.** `build_event_registry` + `chase_damaged_events` в
  `game/events.*` (CTest `event_damage`, `docs/EVENT_DAMAGE.md`): цепочка
  `GENERATE` (опкод 2) → `0x45fa00` → `CAUSE_BLAST_RADIUS_DAMAGE`
  (опкод 75, `0x45775b` → `0x477280`), обход по `damaged_event` (+0x1f4)
  с глубиной 4 и стеком id.
- Корпус: `dm1` — 241 `GENERATE` / 83 blast-опкода → 61 id / 121 blast;
  `level0` — 212 / 78 → 56 / 114. Механизм доказан: `bgchunk1` id `-30071`
  (2 blast’а) 400000+400000 → 398552 за 1 шаг цепочки; циклы ловятся
  (6 из 8), глубина режет 122 вызова на синтетике.
- **Отрицательный результат по поставке:** из 337 пар (уровень,
  `damaged_event`) **0** имеют blast ни прямо, ни в `GENERATE`-цепочке
  (у `destroy_event` — 1 892), поставочные ведут на пустой `-30129`.
- **Гейты:** `ctest` **164/164**, `regress --mirror` **115/115**.

## Ход 2026-10-23 (партия 33: кабина лифта — привязки нет, шов есть)

- **`M6.4.1` срез 4, отрицательный результат с числами:** привязки
  «лифт → объект `.sbw`» в данных **нет**. `Lift::sob` — индекс `SOB_TABLE`,
  кабина создаётся в рантайме (`0x44e800` → `0x4277cd` → `0x4360f1` →
  `0x43b639`). Проверено 45 лифтов на 17 уровнях: совпадение `origin` с
  `lift_transform(0/65535/остановка)` — **0/45** (допуск 1e-6), ближайший
  `f75 == 1` — 313 юнитов, меш `SOBS/<sob>.sob` — 5/45 и только на
  тривиальных квадах.
- Реализован шов `Lift::cabin_sector/cabin_object` (по умолчанию −1) +
  панель/подавление/`update(height/65535)` при явной привязке
  (CTest `lift_collision`, `docs/LIFT_COLLISION.md`): `level14 KELLIFT`
  `pos_delta=(0,0,260)` — подъём точен (drift=0, нормали 2.5e-16), пол
  кабины 760 → 1020; `lifts_unbound = 45/45` — норма поставки.
- Исправлен `MovingCollider::pose`: `f=0, lift=0` побитово равен статике и
  для скользящих панелей. Кабина игрока **не везёт** (`dz = −39.3`) —
  зафиксировано тестом; нужен contact resolve.
- **Гейты:** `ctest` **165/165**, `regress --mirror` **115/115**.

## Ход 2026-10-23 (партия 34: «регенерация» — снята по данным)

- **`M6.1.3` — отрицательный результат с числами.** Опкод **174**
  `DEFINE_REGENERATION_MARKER` (сигнатура `ODD`, `0x45c375..0x45c39f`) есть
  во всех 36 `.sbt`, ровно по одному, всегда `(19, 3000.0, 50.0)`. dword
  уходит в `0x80d878` — SOB-маркер DM-предметов (`0x468840`, живой);
  **оба double в v1.36 мёртвые** (только запись и обнуление загрузчиком
  `0x45713f`/`0x457153`). Регенерации шкал в оригинале нет.
- Механизм реализован как opt-in: `engine/src/game/regen.*` +
  `docs/REGEN.md` (CTest `regen`): задержка, шаг, дробный остаток, без
  перелива; по умолчанию выключен — паритет по числам с `test_sim_damage`
  (`damage=5060`, щиты 6735×4, 120 холостых тиков — без изменений).
- **Гейты:** `ctest` **166/166**, `regress --mirror` **115/115**.

## Ход 2026-10-23 (партия 35: воспроизведение сэмплов и слоты сохранения)

- **`M7.4` срез 2.** `--sample-play NAME [--headless] [--twice]`
  (CTest `cli_sample_play`, `cli_sample_play_twice`, `cli_sample_play_bad`,
  `docs/SAMPLE_CACHE.md` §8): имя → таблица уровня → `SampleCache` →
  SOU0/1/2 → `AudioDevice`. `updown`: frames=6272, rate=16000, bytes=12800,
  `misses=1`; повтор — `hits=1 misses=1`; ошибки (нет имени/записи/битый
  контейнер) — `[error]` и код 2. Нет 3D/громкости/цикла/микшера.
- **`M8.7` срез 1.** `engine/src/ui/save_slots.*` + `docs/SAVE_SLOTS.md`
  (CTest `save_slots`, 103 проверки): 20 слотов по `0x13` (`0x6c2fe0`),
  6 видимых, прокрутка 0..13 (`scroll + 6 < 20` в `0x48aa60`/`0x48abd0`),
  `::: empty slot :::` (`0x4bc3b0`), подтверждение перезаписи как модель.
  Следствие: слот **19 недостижим** — модель это воспроизводит.
  CLI `--save-slots` (CTest `cli_save_slots`): на поставке `present=2/20`,
  `A SAVED GAME`, `::: empty slot :::`.
- **Гейты:** `ctest` **174/174**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 36: строгий писатель сейвов, 3D-сэмпл)

- **`M9.3` (писатель).** `engine/src/res/save_write.*` +
  `docs/SAVE_WRITE_CPP.md` (CTest `save_write`, 125 проверок): проверка
  цепочки блоков, перенос opaque-заголовка `0x10..0x50`, re-parse с
  типизированным сравнением, коды отказа. `saved0`/`saved1` — 243 368 Б
  **побайтово** (sha256 `e39c0c35…`/`93f4edd3…`), 15 блоков; паритет с
  независимым Python-писателем `save_rebuild.py` на исходных и правленых
  файлах. CLI: `--dump SAVEGAME/saved0.sbt --save-out OUT` →
  `blocks=15, byte-exact=yes` (CTest `cli_save_out`).
- **`M7.3` срез 2 (интеграция).** `--sample-play --pos/--listener/
  --volume/--atten`: `--headless` печатает `dist/gain/pan/L/R`, в устройство
  идёт стерео с этими амплитудами (CTest `cli_sample_play_3d*`,
  `docs/AUDIO_3D.md` §13). Пример §8.1 дословно: `dist=198`, `gain=58`,
  `pan=159`, `L/R=23/32`; yaw 1.5708 → `pan=122, L/R=32/29`; далёкий
  источник — `gain=0, L=R=0`; RMS записанного потока — 0.7188 = 23/32.
- **Гейты:** `ctest` **180/180**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 37: рендер страницы пагера)

- **`M8.1` срез 3.** `engine/src/ui/pager_render.*` + `docs/PAGER_RENDER.md`
  (CTest `pager_render`, `cli_pager_png`): страница 320×200 RGBA, глифы из
  атласов `FONTS1/2/4`, координаты строк из `text_layout`; CLI
  `--pager FILE --png OUT`.
- Числа: `help.sme` стр. 2 — 15 строк / 322 глифа / `missing=0` /
  `ink=3905`; весь документ 734 глифа / 8665 ink; `CREDITS.SML` — 5 страниц
  / 970 глифов / 13 838 ink. Детерминизм, `--page 99` и `--all --png` —
  отказы с кодом 2. PNG 320×200 проверен независимым декодером и визуально
  (текст читается).
- Доказано: индексы палитры, используемые этими атласами, побайтово
  совпадают с `GAMEPAL`. Гипотеза: origin страницы `(0,0)`, чёрный фон
  вместо `HELPSCRN`.
- **Гейты:** `ctest` **183/183**, `regress --mirror` **115/115**.

## Ход 2026-10-23 (партия 38: кабина везёт игрока)

- **`M6.4.1` срез 5.** `SimPlayer::set_platform_carry()` +
  `docs/PLATFORM_CARRY.md` (CTest `platform_carry`): зонд динамического пола
  после шага, перенос на `delta` центроида панели до следующего шага, отмена
  при пересечении статики (сэмплирование `radius/2`), сброс на прыжке/
  сходе/смене панели.
- Числа `level14 KELLIFT`: подъём 760 → 1020 (`dz=+260.000000`,
  `on_platform_ticks=30/30`), спуск симметрично; сход — `on_platform=0`;
  в стену — `max_x=151.996948` при пределе капсулы 157 (не проходит);
  с выключенным правилом — прежние `dz = −39.306847` (паритет с
  `lift_collision`) и ни одного лишнего запроса.
- Ограничения: только перенос (распашные/поворотные кабины не везутся),
  одна панель, бокового толчка нет; на поставке панель кабины появляется
  только через `Lift::cabin_sector/cabin_object`.
- **Гейты:** `ctest` **184/184**, `regress --full --mirror` **125/125**,
  `build/warn` — без предупреждений.

## Ход 2026-10-23 (партия 39: найден и исправлен перевёрнутый текст пагера)

- **Баг рендера пагера (мой прошлый вывод «текст читается» был неверным).**
  `pager_render` писал строку 0 глифа последней в ячейке
  (`y + cell_height − 1 − row`) → каждый глиф зеркалился по вертикали, а
  `ink_pixels` не менялся, поэтому тест молчал. Источник истины: `0x47faf0`
  пишет строку 0 в целевую и идёт вниз (`gm/hud.py::draw_text`,
  `docs/HUD_AUDIT.md` §2.9). Исправлено на `y = page_y + line.y + row`.
- **Регрессия теперь ловится:** `test_glyph_orientation` рендерит `<d>L</d>`
  и сравнивает профиль чернил по строкам с растром глифа из атласа.
  Проверено обоими прогонами: с перевёрнутым порядком — 4 расхождения
  (FAIL), с правильным — 0.
- Визуальная проверка после фикса: `help.sme` стр. 2 читается корректно
  («DEFAULT CONTROLS», «Arrow Keys - Move», «PgUp/PgDn - Look up»).
- **`M8.2` срез 2 — рендер меню:** `engine/src/ui/menu_render.*` +
  `docs/MENU_RENDER.md` (CTest `menu_render`, `cli_menu_png`), CLI
  `--menu --png OUT`: панель `PLATE` 196×131, шрифты 0/4/5, подсветка;
  MAIN 7 пунктов / ink 638, CONTROL OPTIONS 10 / 1466, QUIT GAME? 2 / 228;
  все 34 экрана (26 с пунктами) — 29 916 ink; визуально проверено.
- **Гейты:** `ctest` **187/187**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 40: фон HELPSCRN и fade)

- **`M8.1` срез 4.** `--pager --background` кладёт `HELPSCRN` (запись 46,
  320×200, `FRONTEND/BMP.IDX`) под текст; хеш панели побайтово равен
  `flipV(parse_bmp)`, ориентация доказана читаемыми заголовками (`HELP`,
  `LOADING`, `MISSION OBJECTIVES`); найдены также `BRIEFBG2` (14) и
  `LOADPAGE` (56). `--fade N` — кривая `0x682f90` (`k/11`, 0 — чёрный,
  11/15 — тождество); пересчёт цвета в индекс не разобран, поэтому LUT —
  модель `(v·k+5)/11` (макс. 75→34 при `k=5`).
- Старый путь (`--png` без флагов) **побайтово тот же** — проверено на
  `help.sme` стр. 2 (`ink=3905`, sha256 совпал).
- Сабагент независимо нашёл тот же дефект перевёрнутых глифов (по `F`/`L`/`T`)
  и не стал править молча; после моего фикса §5/§6 дока обновлены —
  `test_glyph_orientation` ловит переворот (4 расхождения).
- Визуально проверено: страница 2 читается на реальном фоне `HELPSCRN`
  (лого `HELP`, красная рамка, стрелки), текст не перевёрнут.
- **Гейты:** `ctest` **189/189**, `regress --full --mirror` **125/125**.

## Ход 2026-10-23 (партия 41: загрузка сейва, виджеты меню, DM, оборудование, звуки, апскейл)

Семь сабагентов параллельно; пять влиты в этом ходе (полигоны — следующим).

- **`M8.7`/`M9` — сейв → живое состояние.** `game/save_load.*` +
  `docs/SAVE_LOAD.md` (CTest `save_load`, CLI `--save-load SLOT
  [--save-load-ticks N]`): уровень из заголовка, позиция/yaw/виталы/ammo/
  `SECTOR_STUFF` реально попадают в `GameRuntime` и `SimPlayer`; `saved0`
  → `(1116.174, 798.208, 48.000)`, yaw 5.073615, броня 10000, щиты 8000×4,
  ammo 2000,90, 13/256 активных; 60 тиков детерминированы (два дайджеста),
  15–16 полей в `unsupported` — без выдуманных значений.
- **`M8.2` срез 3 — виджеты.** `menu_render` рисует `SLIDE`/`KNOB`/
  `CHECK`/`NOCHECK`/`MENUARRW`: CONTROL OPTIONS 9 виджетов / ink 5752
  (текст 1466 + 4286), слайдер 0/mid/max — 121/121/126 пикселей разницы,
  чекбокс 128, стрелка (поворот 180°) 56; SOUND/GRAPHICS/PLAY — 3/2/6
  виджетов. Формула бегунка — модель (оригинал едет 54 px от `x+1`).
- **`M9.2` — правила DM.** `game/dm_state.*` + `docs/DM_STATE.md` (183
  проверки): режимы 0..3, флаги `0x01..0x20`, `+1` убийце, team kill `−1`,
  suicide `−1`, респавн **136 тиков**, 8 слотов, гандикап 18 точек —
  0 расхождений с `tools/re/dm_rules.py`.
- **`M6.2.4` — оборудование.** `game/equipment_state.*` (13/12 записей,
  12 доступных, предел 31, события всех 13 записей, `BALLAST` по биту,
  `MINE LAYER` снимает бит при ammo 0).
- **`M7.6` — политика звуков.** `audio/sound_policy.*`:
  `sounds` → `music` → missing, `-1`/`-2`, один `Warn` на ключ, тишина не
  падение; `level0` 137/137/0/0, `dm1` 135/135/0/0.
- **`M8.5` — апскейл/fade.** `render/upscale.*`: nearest 1..8 (5 378 000
  точек, 0 несовпадений), LUT fade (0 — чёрный, 11 — тождество, 1..10
  монотонно). Новый факт: уровни 12..15 **не** почти-тождественны
  (132/73/48/45) — прежняя формулировка опровергнута.
- **Аудит:** + `SAVE_WRITE_CPP`, `PAGER_RENDER`, `MENU_RENDER` — **56/56
  по 22 докам**.
- **Гейты:** `ctest` **197/197**, `regress --full --mirror` **125/125**.

## Ход 2026-10-24 (партия 42: Ghidra — кэш, имена, структуры)

- **Кэш декомпилятора.** `fncache.py`: `invalidate`/`invalidate_dependents`
  (переименование 0x47a980 чистит и 0x45280f, который её вызывает),
  `invalidate_all` (после типов), `warm --file` / `--cached`, `stats --json`,
  `stale`, `selftest`. `mcp.py decompile` для числовых адресов читает и
  пишет кэш (`--refresh`, `--no-cache`); нативные `tools.ghidra.*` — нет.
  Итог: 232 записи / 1409 КиБ, `stale: 0` (было 205/210 без plate-комментария).
- **История запросов.** `tools/ghidra/hot_addrs.txt` — 50 самых частых
  function-start адресов из базы сессий (20–24.10); прогреты, покрытие
  исторических адресов выросло с 61/260 до 232 записей.
- **Имена.** Исправлен битый CSV (literal `\n` склеивал две строки, 9 строк
  с незакавыченными запятыми), добавлены `kind=label` (6 внутренних адресов
  без границы функции), разведена коллизия `damage_thing`: реализация —
  `0x470500`, обёртка — `0x478b30 damage_thing_wrapper`. Добавлены 13
  горячих имён (camera_spring, thing_tick, projectile_tick, thing_destroy,
  chassis_spawn и др.). `apply_names --check --json` — 0 missing.
- **Структуры.** `typegen.py` разбирает `THING_FIELDS` §3/§4 и
  `PLAYER_FIELDS` §3, проверяет размеры 0x528/0x3a8/0x3510 и генерирует
  `gm_types.h`; `apply_types.py` (pyghidra, проект закрыт) импортирует типы,
  создаёт `things[260]`, `thing_types[128]`, `players[8]`, сохраняет ярлыки
  документированных адресов и пишет `docs/re/ghidra/types_report.md`.
  Результат на 5 горячих функциях: `&DAT_…` 994 → 0, поля
  `things[i].flags_b`, `players[i].pose`; 274 типа в проекте.
- **Гейты.** `state.py --check` теперь ловит расхождение имён и `stale` кэш;
  `regress --category names` 1/1; `fncache selftest` ok; `typegen --check`
  ok. Документация: `docs/GHIDRA.md` §3.1/§4.1/§5.2/§5.3, `RE_METHOD` §3/§6/§9,
  `AGENTS.md` (кэш/типы), `RE_AUDIT` §2/§5/§8, `ROADMAP` M2.1.6–M2.1.8.

## Ход 2026-10-24 (партия 42: LOS, поток смерти, звуки UI, экономика, кампания, полигоны)

- **Влито 6 веток** (все — новые файлы + `CMakeLists.txt`):
  `feat/gameflow` (`game/game_flow.*`) — смерть 136 тиков → `respawn_due`
  на 137-м → `alive` на 273-м, трасса FNV-1a `5ea7d56985d1a5b9`;
  `feat/los` (`game/line_of_sight.*`) — центральный луч, `level0` 313.6367
  и стены t=56/72/336/80, `dm1` 2504.3961 и t=826.679/400/1964.061/399,
  батч 10 целей 6/4; `feat/uisounds` (`ui/ui_sounds.*`) — 306 планов по 34
  экранам, 1230 проверок, записи `SOU.IDX` 581/510/511/425/804/636;
  `feat/economy2` (`game/economy_state.*`) — цены/масса `level0`/`dm1`
  сходятся с §12 (1 100 000; 2185/1335), 353 проверки; `feat/campaign2`
  (`game/campaign_state.*`) — счётчик 1→25 по 24 миссиям, 66 сайтов,
  `GUNMETAL.HI` round-trip (576 Б / 9 записей), 258 проверок;
  `feat/poly` (`tools/editor/sbwpoly.py`) — self-test 118/118,
  round-trip 36/36 (235 069 полигонов), правки `level0` 1/11/120/2 байта.
- **Гейты:** `ctest` **202/202** (188 quick + 14 full, 237 с),
  `regress --full --mirror` **125/125** (96,9 с, 6 из кэша),
  `regress --mirror` **115/115**, `build/warn` — 0 предупреждений
  `gm_engine`. Убраны неиспользуемые хелперы из трёх тестов
  (`moving_collider`, `door_collision`, `regen`).
- **Аудит чисел:** + `SBW_POLY`, `CAMPAIGN_STATE`, `UI_SOUNDS_STATE` —
  **59/59 по 25 докам**, `--strict` rc=0.
- **Доки:** исправлены `UI_SCREENS.md` §3 (маскированные блиты одного
  спрайта, шаг строки 0xc4, `0x67f0c8` — буфер `KNOB`) и
  `PAGER_RENDER.md` §2.5 (уровни fade 12..15 не тождественны:
  132/73/48/45 совпадений по дампу `hud_lut.bin`).
- **Запущена партия 43 (4 агента):** `chassis_physics` (M6.1.1),
  `camera_model` (M6.1.6), `pickup` (M6.2.6 срез 3), `collide_grid`
  (broad-phase без изменения результата).

## Ход 2026-10-24 (партия 43–44: шасси, камера, подбор, broad-phase, цели, кадры эффектов, перепись, туман, брифинги, сохранения, фаззинг)

- **Влито 12 веток** (все — новые файлы + `CMakeLists.txt`):
  `feat/chassisphys` (`game/chassis_physics.*`) — 8 `double`
  сверены **616/616** записей 36 уровней, плато 1547.00/1137.50/1365.00,
  `t90` 0.2527/0.3187/0.6374 с, подвеска 0.5 → затухание 17.2×;
  `feat/camera2` (`render/camera_model.*`) — cockpit/chase/orbit, тряска
  seed `0x9E3779B9` (0 на 64-м тике);
  `feat/pickup2` (`game/pickup.*`, 256 проверок) — кламп брони 10000,
  щиты 25/25/25/25, патроны 5000, масса-граница 500;
  `feat/cgrid` (`render/collide_grid.*`) — паритет `500/500` лучей
  (`max_delta=0`) на `level0`/`dm1`, кандидатов 0.79 %/0.56 %, 3.3–4×;
  `feat/objmask` (`game/objectives.*`, 537 проверок) — `declare(0..30)`,
  условие 7, секреты 101/105;
  `feat/fxframe` (`render/fx_frame.*`) — 6117/133/18/6135, 49/48 серий,
  1274 ссылки, 0 неразрешённых; исправлено `EFFECTS.md` §3.1
  (`handle>=1`, а не `link_sector!=0`: 18 против 10);
  `feat/census` (CTest `world_census`) — `28192/235069/357818/1779/3964/45`
  за 2.3 с, два прохода побитово равны;
  `feat/fogcpp` (`render/fog.*`, 99 проверок) — `z=1→62`, `59→4`,
  таблица `0 mismatches/65536`; найден баг оракула: `fog.py` читал
  палитру как `B,G,R` — исправлено, цвета в `FOG.md` §4 поправлены;
  `feat/briefing` (`ui/briefing.*`) — 25 уровней / 58 страниц / 727 строк /
  33 284 глифа, текст 25/25 совпал с Python;
  `feat/menusave` (`ui/save_menu.*`, 131 проверка) — 20 слотов, 6 видимых,
  `first(0,5,6,19)=(0,0,1,13)`, слот 19 недостижим;
  `feat/fuzzsaves` (`tools/qa/fuzz_saves.py`) — **495 кейсов, 0 падений,
  0 зависаний, 0 ASan** (+ ASan-прогон 495/495, 160.6 с).
- **CLI:** `--png-scale N` (1..8) для `--menu/--pager --png` (M8.5):
  `--menu` x3 → 960×600 (2 304 843 Б), пагер x2 → 640×400 (1 024 543 Б);
  `--fade N` теперь и для меню (fade до зума); исправлено `--menu --png`
  без `--menu-keys` (PNG не писался); `--briefing LEVEL [--briefing-text]`
  (M8.3): `level1` = 2 страницы / 20 строк / 910 глифов.
- **Гейты:** `ctest` **215/215** (242 с), `regress --full --mirror`
  **125/125** (82,4 с), `regress --mirror` **115/115**, ASan **216/216**
  (562 с), `build/warn` — 0 предупреждений `gm_engine`,
  `install_audit` — ok, фаззинг форматов **723** кейса 0 падений,
  аудит чисел **63/63 по 29 докам**, `state.py --check` зелёный.
- **Доки:** новые `CHASSIS_PHYSICS`, `CAMERA_MODEL`, `PICKUP_STATE`,
  `COLLIDE_GRID`, `OBJECTIVES_STATE`, `FX_FRAME`, `WORLD_CENSUS`,
  `FOG_CPP`, `BRIEFING`, `SAVE_MENU`, `FUZZ_SAVES`; `AUDIT_NUMBERS` +7
  проверок; `INDEX` перегенерирован.

## Ход 2026-10-24 (партия 45–46: туман, брифинги, сохранения, `.sob`/`.sml`/`.soa`, скролл UV, оборудование, рекорды, иконки, кокпит, выбор уровня)

- **Влито ещё 9 веток:** `feat/fogcpp` (`render/fog.*`, 99 проверок;
  `z=1→62`, таблица `0/65 536` расхождений; найден баг оракула —
  `fog.py` читал палитру как `B,G,R`, исправлено вместе с цветами
  `FOG.md` §4); `feat/briefing` (`ui/briefing.*` — 25 уровней/58 страниц/
  727 строк/33 284 глифа) + CLI `--briefing/--briefing-text`;
  `feat/menusave` (`ui/save_menu.*`, 131 проверка, слот 19 недостижим);
  `feat/fuzzsaves` (`tools/qa/fuzz_saves.py` — 495 кейсов, 0 падений,
  0 зависаний, ASan 495/495); `feat/sobwrite` (round-trip **672/672**,
  правка `TICK.sob` 2 байта, движок видит `bounds 18→60`);
  `feat/smlwrite` (re-encode **151/151** `.sme` побайтово, 131/147 пар,
  14 редакций + 2 CRLF задокументированы); `feat/texanim`
  (`render/texture_anim.*`: 2917/235 069 полигонов со скроллом,
  max 15 224/10 000, 114 пар); `feat/equipact` (`game/equipment_act.*`:
  434 записи, маска `0xfff`, расход `ammo[E8] -= E7*ticks`, одно
  `no_ammo`; вес `BALLAST` 850/2000); `feat/records` (`ui/records.*`,
  155 проверок: минимум по имени `TESTING RANGE=18`, `KELLER CORE=1570`,
  `NATAKA TOWER=7`).
- **`feat/hudicons2`** (`render/hud_icons.*`, 296 проверок): 143 записи
  `BMP.IDX`, 19/19 оружия и 12/12 экипировки 24×24, компас-модель.
- **`feat/cockpit`** (`render/cockpit.*`, 68 проверок): панель 21 196 px
  (0 отличий от блита), иконки/цифры/тяга/компас; CLI `--hud-png` +
  `--hud-weapon/--hud-ammo/--hud-armor/--hud-kills/--hud-speed`
  (320×200, 256 283 Б).
- **`feat/levelsel`** (`ui/level_select.*`, 407 проверок): 36 записей,
  `counter` 1/8/17/25 → 7/15/27/36 доступно, времена из `GUNMETAL.HI`;
  поправка: компаратор `0x48b350` не складывает регистр — «`LEVEL*`
  первыми» на строчных именах не работает.
- **Гейты:** `ctest` **230/230** (237,4 с), `regress --full --mirror`
  **125/125** (79,9 с), `regress --mirror` **115/115**, ASan **226/226**
  на предыдущей партии (повторный прогон идёт), `build/warn` —
  0 предупреждений `gm_engine`, `install_audit` ok, фаззинг форматов
  723 кейса, фаззинг сейвов 495/495, аудит чисел **66/66 по 32 докам**.
- **Доки:** + `FOG_CPP`, `BRIEFING`, `SAVE_MENU`, `FUZZ_SAVES`, `SOB_WRITE`,
  `SML_WRITE`, `TEXTURE_ANIM`, `EQUIPMENT_ACT`, `HI_SCORES`,
  `HUD_ICONS_CPP`, `COCKPIT`, `LEVEL_SELECT`, `WORLD_CENSUS` (итого 12
  новых); `INDEX` перегенерирован.

## Ход 2026-10-24 (партия 47–48: `.soa`/`.sub`, KILLED, плейлисты, конфиг, объёмные запросы)

- **Влито 6 веток:** `feat/soawrite` (`tools/editor/soawrite.py` — round-trip
  **104/104** `.soa`, 8 573 Б, 839 имён; правка `Flash3.soa` кадр 2 → 98 мс
  = 2 байта, C++ видит `period: 38 (34 ticks)`); `feat/sub` (`res/sub.*` —
  119/64/161 строк, `level0` 15 подстановок из 381 текстуры; важная
  поправка: `.sub` — это `weight + имена`, а не индексы, `20`/`16` — класс
  RAM; `FORMATS.md` §8.2 исправлен); `feat/killed` (`ui/killed.*` —
  `.skb` 2338 Б/69 строк, `.skk` 1967 Б/50, фильтр `0<strlen<0x20` =
  1..30 видимых байт, 138 замен `%S`, `pick = seed % n`; CLI `--killed`,
  `--killed-all`, `--killed-seed`: seed 999 → «PLAYER SENDS PLAYER TO THE
  BONEYARD.», 69 строк); `feat/playlist` (`audio/playlist.*` — 651 `.sog`
  (598+53), `MUSIC/SOU.IDX` 3092, 5211 имён → 5088 резолвится, 123
  вхождения/95 имён/25 песен отсутствуют, 998 дубликатов); `feat/cfg`
  (`game/game_cfg.*` — 8192 Б round-trip, шапка `av1.93`+`0x1A`, 16 полей:
  `music=16 effect=20 800x600x8 border=3 gamma=1 light_maps=2 camera=1`,
  13 сеттеров, ≤2 байт на правку); `feat/overlap`
  (`render/collide_overlap.*` — SAT по 13 осям + расстояние до
  треугольника; паритет 2000/2000 синтетики, 240/240 коробок и 120/120
  сфер на `level0`/`dm1`; полы/стены/потолки 558/2082/522; найден баг
  Мёллера–Трамбура на ребре в плоскости).
- **Гейты:** `ctest` **237/237** (264,7 с), `regress --full --mirror`
  **125/125** (110,5 с), `regress --mirror` **115/115**, аудит чисел
  **71/71 по 37 докам**, `state.py --check` зелёный, ASan — 231/231 на
  предыдущей партии, повторный прогон идёт.
- **Доки:** + `SOA_WRITE`, `SUB_CPP`, `KILLED`, `PLAYLIST`, `GAME_CFG`,
  `COLLIDE_OVERLAP`; `FORMATS.md` §8.2 (`.sub`/`.cfg`), `ROADMAP` M7.5/
  M12.5/M6.4.1/M11.5.

## Ход 2026-10-24 (партия 49: точные fade-таблицы, save-action, `.sog`/`.sof`/`.soo`)

- **Влито 5 веток:** `feat/hudlut` (`render/hud_lut.*` — дамп `0x682f90`
  4096 Б, FNV `6330406254771f3d`; уровни 12..15 — **132/73/48/45**
  индексных тождеств, 11 — 254/256; модель `fade_rgba` совпадает только
  на концах: 0 → 236/256, 11 → 256/256); `feat/saveaction`
  (`game/save_action.*`, 76 проверок: `save` слота 0 → 243 368 Б,
  sha256 `e39c0c35…`, `load` → `level0`/`(1116.173752,798.207621,48.0)`;
  запись в установку — `refused_write` с неизменным sha256);
  `feat/sogwrite` (`tools/editor/sogwrite.py` — round-trip **651/651**
  (598 `.sog` + 53 `.SOG`), 43 205 Б, self-test 41/41; правка
  `Dm1A.sog` → `winner` 6 байт, C++ `--playlist` видит её);
  `feat/sofwrit` (`.sof` — round-trip **48/48**, 95 330 304 Б,
  11 637 кадров; правка пикселя = 1 байт, `--film` FNV меняется);
  `feat/soowrite` (`.soo` — **40/40**, 3 416 Б, 264 кадра; правка
  `dur2=70` = 1 байт, `--anim` меняет PNG).
- **CLI:** `--best-times` (9 строк `GUNMETAL.HI`, 7 имён, минимум
  `TESTING RANGE=18`), `--playlist FILE` (`Dm1B.sog`: 8/7/1,
  236 876 Б), `--killed`/`--killed-all`/`--killed-seed` (ранее).
- **Гейты:** `ctest` **243/243** (266 с), ASan **244/244** (549,5 с),
  `regress --full --mirror` **125/125** (93,3 с), `regress --mirror`
  **115/115**, `build/warn` — 0 предупреждений `gm_engine`,
  `install_audit` ok, фаззинг сейвов 0 падений, аудит **75/75 по
  41 докам**, `state.py --check` зелёный.
- **Доки:** + `HUD_LUT`, `SAVE_ACTION`, `SOG_WRITE`, `SOF_WRITE`,
  `SOO_WRITE`; `INDEX` перегенерирован.

## Ход 2026-10-24 (партия 50: PNG брифинга, `.soe`/`.sni`)

- **Влито 2 ветки:** `feat/briefpng` (`ui/briefing_render.*` — кадр
  320×200 через пагер; **0 отличающихся пикселей** от прямого
  `render_page`; корпус 25 брифингов = 58 страниц/727 строк/33 284 глифа,
  `missing=0`, ink 344 505; `level0a` страницы различаются 3216/2333/3279 px,
  `--background` меняет 61 786 px, fade 11 — тождество, 0 — чёрный);
  `feat/soewrite` (`tools/editor/soewrite.py` — round-trip **2/2**
  (`.soe` 340 Б/67 токенов, `.sni` 3 Б), self-test 51/51; правка токена 47
  `200→250` = **1 байт**, C++ `parse_soe` видит 250).
- **CLI:** `--briefing LEVEL --png OUT [--page N] [--background] [--fade K]
  [--png-scale N]` — `level1` стр. 1: `15 строк / 719 глифов / ink 7214`,
  PNG 256 283 Б; CTest `cli_briefing_png`, `_page2`, `_bad_page`, `_scale`.
- **Гейты:** `ctest` **248/248** (262,8 с), ASan **249/249** (547,1 с),
  `regress --full --mirror` **125/125** (93,8 с), `regress --mirror`
  **115/115**, аудит **76/76 по 42 докам**, `state.py --check` зелёный.

## Ход 2026-10-24 (партии 51–54: экраны 13/14/17, архивы SBM/BMP, HI/KILLED/KEY)

- **Экраны фронтенда в PNG:** `ui/level_select_render.*` (экран 17, плата
  `PLATE` 196×131, окно `clamp(selected-5,0)`, `ink=1305`), `ui/save_menu_render.*`
  (экран 14: 6 строк, `y=28` шаг 12, `ESC (38,101)`, `ink=917`),
  `ui/start_game_render.*` (экран 13: 8 строк, пункты `(35,23)`/`(35,33)`,
  слоты `y=44+11i`, `ESC (35,110)`, `ink=1608 arrows=1`); стрелки `MENUARRW`
  доказаны (`0x485cc0` вниз / `0x485d00` вверх, колонка 159, строки 2/121,
  41 px). CLI: `--level-select`, `--save-menu`, `--start-game`.
- **Действие сейва:** `--save-action save|load` (слот 0 → `level0`,
  `feet=(1116.173752,798.207621,48.0)`, `blocks=15`, `sector_stuff=13`;
  запись 243 368 Б, sha256 `e39c0c35…`).
- **Писатели:** `sbmwrite.py` (2 архива, 1827 записей, 46 525 219 Б,
  round-trip 2/2), `bmpwrite.py` (`FRONTEND/BMP.IDX`: 143 записи,
  2 845 582 Б, round-trip 1/1, extract/replace), `hiwrite.py`
  (`GUNMETAL.HI` 576 Б = 9×0x40, round-trip 1/1), `killedwrite.py`
  (`.skb`/`.skk` round-trip 2/2, 69/50 принятых строк), `keywrite.py`
  (3 `.key` по 376 Б, round-trip 3/3; хвост 112 Б — гипотеза).
- **Гейты:** `ctest` **266/266** (266,6 с), `regress --full --mirror`
  **125/125**, аудит **81/81 по 47 докам**, `install_audit` ok, ASan —
  прогон после влития (прошлый 263/263), `state.py --check` зелёный.

## Ход 2026-10-24 (партии 55–56: экран 30, точный fade, строитель LUT, things)

- **Экран 30:** `ui/keycfg_render.*` (31 строка/шаг 8/`x=5`, окно
  `max(0,cursor-5)` ≤ 21, стрелки 41 px; `PERFECT` — 29 имён, ink 2659),
  CLI `--keycfg NAME --png OUT` + 4 CTest.
- **Точный fade:** `PageRenderOptions::fade_lut*` + `BriefingRenderOptions`
  (`--pager/--briefing --fade-lut N`): уровень 11 — **0 отличий** от кадра
  без fade (пагер и брифинг), 12..15 различаются (пагер 40 747/56 593/45 025;
  брифинг 39 238/52 240/46 118), модель по-прежнему клампит; CTest
  `cli_fade_lut` через `cmake -E compare_files`.
- **Строитель `0x682f90` разобран:** `tools/re/lut_build.py` строит
  **4096/4096** байт (FNV `0x6330406254771f3d`, self-test 59/59): `k/11`
  double, усечение, 6-битная палитра, 20 слотов исключений. Дамп больше не
  единственный источник.
- **Связка things:** `world/thing_anim.*` (клип = `Sobanim/<model>.soo`,
  кадр через `soo_pose_at_tick`; 40 файлов/261 загружаемый кадр, `level13`
  — 10 из 267 things).
- **CLI-док:** `docs/CLI.md` + полное покрытие `--help` (123 опции; раньше
  11 опций `--hud*`, `--lm-world`, `--spawn-*` отсутствовали в подсказке).
- **Гейты:** `ctest` **273/273** (276,6 с), ASan **274/274** (597,2 с),
  `regress --full --mirror` **125/125**, аудит **84/84 по 50 докам**.

## Ход 2026-10-24 (партии 57–59: fade во всех кадрах, FILESLOT, редактор наборов)

- **Точный fade во всём фронтенде:** `fade_lut*` добавлен в `menu_render`,
  `level_select_render`, `save_menu_render`, `start_game_render`,
  `keycfg_render`; CLI `--fade-lut K` общий для `--menu/--level-select/
  --save-menu/--start-game/--keycfg/--pager/--briefing`. Уровень 11 — **0
  отличий** от кадра без fade на всех 7 кадрах, 12..15 различаются (экран 13:
  17 315/22 823/20 803); `cli_fade_lut` и `cli_fade_lut_built` (без дампа,
  таблица из `GAMEPAL`) через `cmake -E compare_files`.
- **C++-строитель LUT:** `build_hud_lut()` — 4096/4096 от дампа; уточнено,
  что `0x40e0e0` меняет только RC (PC=53), поэтому double-модель точна.
- **FILESLOT:** полосы 124×10 на экранах 13 `(35,43+11i)` и 14
  `(38,27+12i)`; 6708 изменённых пикселей на 6 полос, `ink` не меняется.
- **Экран 30:** курсор/окно `max(0,cursor-5)`/`top ≤ 21` (CLI
  `--keycfg-cursor`) и редактор 34/35 (подсказка `Enter changes, Delete
  clears`, подсветка ячейки, конфликт `%s has been used twice` в `y 109`).
- **Модуль HUD-overlay:** `render/hud_overlay.*` (261 проверка, паритет с
  прежним `overlay_hud` побайтово; `main.cpp` переведён на модуль).
- **Golden-сценарии `--sim`:** 5 сценариев/9 прогонов, детерминизм 9/9,
  `docs/SIM_SCENARIOS.md`.
- **Гейты:** `ctest` **280/280** (271,9 с), ASan (прогон после влития),
  `regress --full --mirror` **125/125**, аудит **84/84 по 50 докам**.

## Ход 2026-10-24 (партия 60: экраны 6/21/33/37 + CLI и fade на 13 кадрах)

- **Новые кадры:** `ui/local_players_render.*` (экран 21: 3 пункта по две
  строки 27/36, ink 936), `ui/quit_render.*` (33: `QUIT GAME?` 2 строки
  ink 228 / `QUIT OPTIONS` 3 строки ink 576), `ui/video_mode_render.*`
  (37: `Yes`/`No` 27/43, ink 356), `ui/restart_render.*` (6: 2..4 пункта
  `x 10`, `y=27+16i`, ink 585/898/1132).
- **CLI:** `--local-players [--local-players-cursor N]`, `--quit
  [--quit-in-game] [--quit-cursor N]`, `--video-mode [--video-mode-cursor N]`,
  `--restart [--restart-items N] [--restart-cursor N]`; всего **142 опции**
  парсера, все в `--help`.
- **Точный fade:** `cli_fade_lut` теперь сверяет **13 кадров** (уровень 11 =
  побайтово без fade, 13 ≠ без fade), в том числе без дампа
  (`cli_fade_lut_built`).
- **Гейты:** `ctest` **301/301** (264,2 с), `regress --full --mirror`
  **125/125** (179,8 с), аудит **84/84 по 50 докам**, ASan — прогон после
  влития.

## Ход 2026-10-24 (партия 61: экраны 18/22, `--things-anim`, 15 кадров fade)

- **Экран 18** (`ui/video_mode_list_render.*`): `SELECT VIDEO MODE` (y 11),
  `Press Esc to close` (y 20), окно 10 строк `y = 29+9i`, стрелки 41 px;
  таблица 15 режимов — **модель** (`EnumDisplayModes`+Glide, доказаны лишь
  правила/тексты). ink 2838.
- **Экран 22** (`ui/mouse_setup_render.*`): 12 строк `y = 24+8i`, 3 слайдера
  `SLIDE`/`KNOB` с доказанной арифметикой `108 − trunc((v−0.2)·(−27))`
  (кноб 108..162), ink 3986.
- **CLI:** `--video-mode-list [--video-mode-list-cursor N]`,
  `--mouse-setup [--mouse-setup-cursor N]`, `--things-anim [--level NAME]
  [--things-anim-tick N] [--things-anim-limit N]` — живой рантайм
  `Things::load` + `Ai::attach`, `level13` при тике 210:
  `things=268 agents=140 bound=10 unbound=258 animated=2 still=8`,
  первая привязка `SLUMP → slump frame=8 looping=1`. Всего **149 опций**.
- **Точный fade:** `cli_fade_lut` проверяет **15 фронтенд-кадров** (11 =
  без fade побайтово, 13 ≠), в том числе на собранной из `GAMEPAL` таблице.
- **Гигиена:** `build/warn` — **0 предупреждений** (починены 11 в тестах
  фронтенда).
- **Гейты:** `ctest` **309/309** (273,3 с), `regress --full --mirror`
  **125/125** (102,7 с), аудит **84/84 по 50 докам**, ASan — прогон после
  влития.

## Ход 2026-10-24 (партия 62: экраны 26/27, 17 кадров fade)

- **Экран 26** (`ui/joystick_centering_render.*`) — это `JOYSTICK CENTERING`,
  а не «mission info»: модуль переименован после влития (коммит `9d3f9fd`);
  5 статических строк (25/35/45, 86/97; x 36/41), ink 1141; курсор 0 белит
  весь текст (item 0 у всех строк) — квирк `0x485dd0`.
- **Экран 27** (`ui/match_setup_render.*`): `MULTIPLAY GAME OPTIONS` (y 11),
  8 строк (тип y 22, cash y 32, чекбоксы 45/60/75/90/103, `START GAME!`
  y 113), 5 чекбоксов по флагам `0x75`; слайдеров нет (в `0x4885c0` нет
  `SLIDE`/`KNOB`). ink 2239, все галочки — 2362.
- **CLI:** `--joystick-centering [--joystick-centering-cursor N]`,
  `--match-setup [--match-setup-cursor N] [--match-setup-type N]
  [--match-setup-cash N] [--match-setup-flags HEX]`; всего **156 опций**.
- **README:** добавлен раздел «Движок (C++)» с командами сборки, примерами
  CLI и текущими счётчиками регрессии (115/125).
- **Fade:** `cli_fade_lut` — **17 фронтенд-кадров**.
- **Гейты:** `ctest` **316/316** (275,9 с), `regress --full --mirror` — прогон
  после влития, аудит **84/84 по 50 докам**.

## Ход 2026-10-24 (партия 63: экраны 16 и 9, 19 кадров fade)

- **Экран 16** (`ui/key_jamming_render.*`): `KEY JAMMING` (y 14) + три
  подсказки (26/35/44) + имена зажатых клавиш сеткой `x = 30+34·(n%4)`,
  `y = 54+8·(n/4)` (≤ 28); «джемминга» как логики в оригинале нет — это
  пассивный вывод массива `0x4bf2a8`. ink 752, 5 клавиш — 939.
- **Экран 9** (`ui/ingame_menu_render.*`): у экрана **нет меню** — ветка
  state 9 выходит до диспетчера; кадр = одна полноэкранная запись
  `DEMOTION`/`DISBAND` 320×200 (выбор имени `0x497210`), 49 323 из 64 000
  пикселей различаются.
- **CLI:** `--key-jamming [--key-jamming-keys 30,31] [--key-jamming-cursor N]`,
  `--ingame-menu [--ingame-menu-bg DEMOTION|DISBAND]`; всего **161 опция**.
- **Fade:** `cli_fade_lut` — **19 фронтенд-кадров**.
- **Гейты:** `ctest` **322/322** (269,8 с), `regress --full --mirror` — прогон
  после влития, аудит **84/84 по 50 докам**.

## Ход 2026-10-24 (партия 64: экраны 15 и 20, `--hud-lut-dump`, 21 кадр fade)

- **Экран 15** (`ui/save_name_render.*`): `SAVE GAME` (y 14), `ESC TO CANCEL`
  (y 101), строка ввода (42, 28+12·row) с хвостовой кареткой `'-'` (литерал,
  без мигания), буфер 17 симв. + каретка, `FILESLOT` 124×10 под строкой;
  ink 544.
- **Экран 20** (`ui/lay_bucks_render.*`): shareware-заглушка `LAY DOWN THE
  BUCKS` (14) + `THIS FEATURE IS NOT` / `AVAILABLE IN THE` / `SHAREWARE
  VERSION` (27/36/45); ни суммы, ни курсора, ни `SLIDE`/`KNOB`; ink 1036.
- **CLI:** `--save-name [--save-name-text NAME] [--save-name-row N]
  [--save-name-no-caret] [--no-fileslot]`, `--lay-bucks`; всего **168 опций**.
- **LUT:** `--hud-lut-dump OUT` строит те же 16×256 таблиц (4096 Б,
  FNV `0x6330406254771f3d`, `cmp` с дампом пуст) и отказывается писать внутрь
  каталога установки.
- **Fade:** `cli_fade_lut` — **21 фронтенд-кадр**.
- **Гейты:** `ctest` **331/331** (275,4 с), `regress --full --mirror` — прогон
  после влития, аудит **84/84 по 50 докам**, `build/warn` — 0,
  `install_audit` ok, `fuzz_saves` ok.

## Ход 2026-10-24 (партия 65: экраны 5 и 36, карта покрытия, 23 кадра fade)

- **Экран 5** (`ui/error_render.*`): `ERROR` (y 14), сообщение словами с
  (10, 34), перенос при `x+width > 180`, шаг 10, `ESC TO RETURN TO MENU`
  (y 91); подстановка `%s`/`%ld` делается до экрана (`sprintf` в буфер
  `0x4aa2f8`). `No Error.` — ink 489, `Unable to load world file
  (savegame).` — 803. CLI `--error [--error-message TEXT]`.
- **Экран 36** (`ui/player_setup_render.*`): `PLAYER 1/2 JOYSTICK SETUP`
  (y 11) + подсказка (y 20); 1 строка без устройства / 43 с ним, окно 10,
  `x 13`, `y = 28+9i`, квирк «Hat 2 Y +13 px». CLI `--player-setup
  [--player-setup-cursor N] [--player-setup-joystick N]
  [--player-setup-player N]`.
- **Документация:** `docs/UI_COVERAGE.md` — карта «экран → модуль → CLI →
  тест → числа» для 20 экранов и общих кадров (`--menu`, `--pager`,
  `--briefing`, HUD).
- **Fade:** `cli_fade_lut` — **23 фронтенд-кадра**; всего **174 опции** CLI.
- **Гейты:** `ctest` **338/338** (244,9 с), аудит **84/84 по 50 докам**,
  `build/warn` — 0, `install_audit` ok, `fuzz_saves` ok; ASan — прогон
  после влития волны 66.

## Ход 2026-10-24 (партия 66: экраны 10/11/12/32/24/25/31, 29 кадров fade)

- **Экраны 10 и 11:** `ui/control_options_render.*` (10 строк `y = 27+9i`,
  `x 20/32`, ink 1736) и `ui/sound_options_render.*` (7 строк `y = 27+10i`,
  2 слайдера `x = 69 + trunc(27·v/10)`, чекбокс; off-раскладка `rows=1
  ink=1098`, low memory `sliders=1`).
- **Экраны 12 и 32:** `ui/graphics_options_render.*` (6 строк, слайдер
  `Border` `x = 89+6·b`, чекбокс `Reflections`, ink 1866) и
  `ui/play_options_render.*` (8 пунктов, 6 чекбоксов битами, `DYNAMIC
  CAMERA` 0..2, in-game строка шрифтом 8, ink 2765).
- **Экраны 24/25 и 31:** `ui/player_name_render.*` (заголовки на y 14, имя
  `FONTS2` 16×16 по центру y 28, каретка 8×17, лимит 10, ink 1812/2331) и
  `ui/new_keyset_render.*` (`NAME OF NEW KEY SET` y 14, имя шрифтом 3,
  каретка-рамка 8×17, лимит 15, пустое → `NEW SET`, ink 1849).
- **CLI:** `--control-options`, `--sound-options`, `--graphics-options`,
  `--play-options`, `--player-name`, `--new-keyset` — всего **207 опций**.
- **Fade:** `cli_fade_lut` — **29 фронтенд-кадров**; `build/warn` — 0.
- **Гейты:** `ctest` **361/361** (280,8 с), ASan **362/362** (704,7 с),
  `regress --full --mirror` **125/125** (116,1 с), аудит **84/84 по 50
  докам**, `install_audit` ok, `fuzz_saves` ok, `state.py --check` зелёный.
- **История:** master между партиями был свёрнут в один squash-коммит
  (внешнее действие), дальше работа идёт от него; содержимое сохранено.
