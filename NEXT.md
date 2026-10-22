# NEXT — очередь автономной работы

Обновляется в конце каждого хода. Если пользователь не написал `STOP`/`пауза`,
агент берёт верхний незаблокированный пункт и работает дальше без разрешения.

## Где что

- Журнал прошлых ходов (перенесён из этого файла 2026-10-22) — `docs/JOURNAL.md`.
- Аудит процесса, узкие места и очередь задач — `docs/RE_AUDIT.md`.
- Снимок состояния (HEAD, roadmap, тесты, кэш) — `python3 tools/qa/state.py`.
- Здесь — только очередь и следующий шаг; длинные отчёты сюда не писать.

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
   `burn/status` зеркалятся. **`aim_point` уже доходит до автоматов**:
   `Ai::apply_aim(Things&)` раздаёт его через `Things::aim_motion_at()`
   (ошибки `0x473a00` в системе тела: `yaw_error` = пеленг, `pitch_error = 0`,
   `vertical_error = Δz`); потерянная цель очищает её. Осталось: навигация,
   реальное оружие/урон игроку и вызов `apply_aim` из игрового тика.
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
5. **Полный регресс — зелёный.** Python: `regress.py` 113/113 (~46 с на
   зеркале), `--full --mirror` 123/123 (~60–89 с, кэш). C++: `ctest
   -L quick -j 8` 50/50 за ~34 с, полный 61/61 за ~235 с. Причина прежних
   падений (`core`/`camera_rig`/`sbtvm`/`vm`/`things_full`) — `level1.sbt` и
   `level2.sbt` в установке поменяны местами относительно манифеста
   (fnv `2aa7…`/`7e6d…`); тесты теперь адресуют `.sbt`/`.sbw` по содержимому
   (`engine/tests/game_content.h`, `tools/re/game_content.py --check`).
   Голдены не ослаблены, игра не тронута. Слои/бюджеты —
   `docs/REGRESSION.md`.

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

10. **`SECTOR_STUFF` перенесён полностью** (включая `SET_SOURCESECTOR_*` /
    `SET_HANDLESECTOR_*` — см. «Последний ход»). ✅ `.sbt`-запись 8 —
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
    `+0x18`/`+0x54` без читателей; потребители (`0x470c70`, `0x453b50`,
    `0x46c5c0`) и рантайм-массив секторов `.sbw` (`0x5670d8`) не подключены.
    Карта — `docs/RECORDS.md` §2.

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
- **Следующий шаг:** очередь без изменений — пункт 1 (ввод игрока в цикле и
  передача `state.charge` в `fire(...)`). Блокеров нет.

## Стоп-условия

- Пользователь написал `STOP`/`пауза`.
- Все пункты выше закрыты или помечены `⛔` с причиной.
- Нужно необратимое действие/секрет, которого нет.
