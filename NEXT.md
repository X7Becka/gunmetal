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
5. **Полный регресс.** `ctest` 45/53: 8 падений — внешние `level1/2` swap,
   `camera_rig` (правки пользователя) и `controls` (KEYCFG); `regress.py`
   121/121 (`--full`, 224.8 с). Targeted `weapons`/`things`/`thing_ai`/
   `events` — 4/4. Код-регрессий нет.

6. **Ghidra-карты структур (M2.3).** Сделано: `M2.2.1` ✅ (`docs/INIT.md`,
   `tools/re/init_flow.py`), `M2.3.1` 🟡 (`docs/PLAYER_FIELDS.md`),
   `M2.3.3` 🟡 (`docs/THING_FIELDS.md`; заодно исправлен `SOE +0x010`:
   `speed`, не `off_z` — C++/Python/test/regress согласованы), `M2.3.5` 🟡
   (`docs/WEAPON_FIELDS.md`; `charge_rate` в памяти = файл × 0.91, уже в
   `Weapons::load`). В работе: `M2.3.2` (мир/сектор/полигон), `M2.3.4`
   (дверь/переключатель/лифт), `M2.3.7` (звук). Не начато: `M2.3.6`
   (скриптовая таблица сущностей), `M2.2.3` (загрузка/выгрузка уровня).
7. **FORMATS.md ждёт своей очереди.** В рабочем дереве есть незакоммиченные
   правки пользователя в `docs/FORMATS.md`, поэтому уточнения из
   `PLAYER_FIELDS`/`WEAPON_FIELDS`/`THING_FIELDS` туда не внесены. Список:
   `+0x18` игрока — углы (не позиция), `+0x2ff8` — точки навесных частей,
   шасси model index `+0x154` (не `+0x28`), `charge_rate × 0.91`,
   `muzzle = -1`, `THING_TYPES+0x010 = speed`. Внести после того, как
   пользователь закоммитит свои изменения.
8. **Ввод игрока → выстрел.** Остаток пункта 1: курок/слот в игровом цикле
   (`input/controls.*` → `update_charge` → `fire(..., state.charge)` →
   `Projectiles`), плюс выбор оружия. API заряда готов и покрыт тестом.

## Стоп-условия

- Пользователь написал `STOP`/`пауза`.
- Все пункты выше закрыты или помечены `⛔` с причиной.
- Нужно необратимое действие/секрет, которого нет.

## Последний ход

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
