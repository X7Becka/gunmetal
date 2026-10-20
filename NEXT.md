# NEXT — очередь автономной работы

Обновляется в конце каждого хода. Если пользователь не написал `STOP`/`пауза`,
агент берёт верхний незаблокированный пункт и работает дальше без разрешения.

## Сейчас

1. **Impact/THING → `.sbt`.** Подключить event/impact-ветки к `Weapons::load`:
   `GRENADE`, `ROCKET`, `RGM`, `MRL`, `TAC NUKE`, `PLASMA`, `EMP`,
   `DISINTEGRATOR`; убрать остатки `kDefaultProjectileSpeed` из реального пути.
   Проверка: `weapons` + новые golden’ы по `level0.sbt`/`level10.sbt`.
2. **AI wiring.** Навигация/240-кадровая история цели по `target_history_offset`;
   использовать `Thing::health/shields/hits/events` в runtime.
3. **Event effects.** Реестр текста `0x619860`, побочные эффекты switch,
   сетевой event-блоб (`u32 len + bytes`).
4. **Player integrator.** Проверить `0x81bfb8` (`v = v*k + step`) в отладчике:
   согласовать `op4` как ускорение/максимум и `ControllerConfig`.
5. **Полный регресс.** `ctest` (52), `regress.py --full`, обновить baseline.

## Стоп-условия

- Пользователь написал `STOP`/`пауза`.
- Все пункты выше закрыты или помечены `⛔` с причиной.
- Нужно необратимое действие/секрет, которого нет.

## Последний ход

- Impact-отчёт прочитан; добавлены `Blast`, `apply_blast`, `apply_thing_damage`,
  исправлен `ShieldSide`, предпочтение воздушной ветки `FIRE_*`.
- `weapons` (1/1), `things` (1/1), `thing_ai` (1/1), полный CTest 52/52
  (до последней правки `thing_type`; targeted после неё — 1/1).
- Следующий шаг — пункт 1.
