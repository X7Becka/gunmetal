# Death, respawn and "game over" in the engine (`M9.5`, slice 1)

**Статус:** модуль `engine/src/game/game_flow.{h,cpp}` + тест `game_flow`
(`engine/tests/test_game_flow.cpp`, CTest label `quick`, 170 проверок).
Чистый автомат: без `GameRuntime`, VM, сейвов, часов, RNG, рендера и меню.
Факты — из `docs/GAME_OVER.md` (сняты с `gundx.exe` только на чтение,
инструмент `tools/re/gameover.py`, `--self-test` 120/120); эталон поведения —
`docs/DM_RULES.md` §4.3 и `engine/src/game/dm_state.h`.

Проверка:

```sh
cmake --build build --target gm_game_flow_tests -j 4
./build/engine/tests/gm_game_flow_tests        # game_flow: ok (170 checks)
ctest --test-dir build -R '^game_flow$' --output-on-failure
```

## 1. Что доказано (код игры)

| факт | значение | источник |
|---|---|---|
| смерть | `action_state = 7` (`player+0x3210`), `death_timer +0x33d4 = 0` | `0x453763`/`0x45372f`, `GAME_OVER.md` §2.1 |
| таймер смерти | `+= dt` каждый кадр состояния 7 | `0x443849`..`0x443853`, §2.1 |
| порог респавна | `death_timer >= 0x88` = **136** тиков → `0x452670` | `0x443b0a`..`0x443b1e`, §2.1 |
| частота тика | 91 Гц (`Sleep(11)`); 136 тиков ≈ **1.49 с** | `docs/PLAYER_GHIDRA.md`, §2.1 |
| респавн в кампании | `SET_PLAYER_START` в том же уровне | §2.1, §2.2 |
| отдельного экрана `GAME OVER` | **нет** (0 строк, 0 упоминаний в установке) | §0.1, §2.2 |
| счётчика жизней/смертей | **нет** | §2.2 |
| автосейва при смерти | **нет** (3 сайта записи: меню + Quick Save) | §2.2 |
| счётчик миссий при смерти | не меняется, респавн в том же уровне | §2.2 |
| «game over»-экран | меню `RESTART GAME` (frontend state 6) | §0.2, §4.1 |
| открытие меню при смерти | Activate (бит `0x8000`) → `state = 6` | `0x443af9`, §2.1 |
| Esc из уровня в то же меню | `state 9 → 6` | `0x48b54c`, §1/§4.1 |
| пункты меню | Reload saved / Restart same R.P.V. (`0x4428f0(0)`) / Restart from HQ (`0x4428f0(1)`) / Quit to title | §4.1 |
| надпись при смерти | `PRESS ACTIVATE TO RESTART` (строка `0x4bc2c8`) | §2.3 |
| смерть в DM | тот же 136-тиковый таймер и `0x452670` | `DM_RULES.md` §4.3 |

Следствия для автомата: **смерть — это таймер, а не состояние конца игры**;
игра после таймера продолжается в том же уровне; единственный «финальный»
экран — меню, которое открывается по Activate (пока мёртв) или по Esc.

## 2. Что модель (не доказано кодом)

Каждое место помечено `MODEL` в `game_flow.h`/`game_flow.cpp`.

1. **`lives`.** В игре счётчика жизней нет (§2.2). `-1` (по умолчанию) —
   «без лимита», это ровно доказанное поведение (каждая смерть → респавн).
   `lives >= 0` — opt-in правило движка: смерть списывает одну жизнь, и если
   после неё осталось 0, истечение таймера уводит в `restart_prompt` вместо
   респавна.
2. **`campaign`.** Гейт только для `lives`; в оригинале кампания и DM ведут
   себя одинаково (§2.1). При `campaign = false` бюджет жизней не тратится и
   `restart_prompt` не наступает автоматически.
3. **`respawn_ticks` / состояние `respawning`.** В игре респавн вызывается
   **сразу** на тике истечения таймера (`0x452670` в `0x443b1e`); второго
   ожидания нет. `respawning` — собственное окно модуля («респавн идёт»), а
   значение 136 просто повторяет доказанную величину таймера смерти.
4. **Направление счётчика.** Здесь таймер смерти идёт вниз, в игре
   `player+0x33d4` растёт до `0x88` (§2.1). Доказана только величина (136) —
   та же оговорка, что в `docs/DM_STATE.md` §2.3.
5. **`prompt_restart()`.** Путь Activate-пока-мёртв (`0x443af9`) без его
   гейтов (`[0x83d620]`, `[0x83d940]`, чит `regen` `[0x5a46d8]`, §2.1/§7.3) и
   без самого меню: только переход в `restart_prompt`.
6. **`restart()` / `return_to_hq()`.** Пункты меню `0x4428f0(0/1)`
   загружают уровень и копируют снимок игрока `0x3510` байт (§4.2). Здесь —
   только переход, сброс таймеров, восстановление `lives` и счётчик вызовов.
7. **Счётчики `deaths`/`respawns`/`restarts`/`hq_returns`** — диагностика
   модуля, в игре их нет (§2.2).

## 3. API (`game/game_flow.h`)

```cpp
enum class PlayerFlow { alive, dying, dead, respawning, restart_prompt };
inline constexpr int kFlowDeathTicks   = 136; // 0x443b0a, §2.1
inline constexpr int kFlowRespawnTicks = 136; // MODEL, та же величина

struct FlowConfig { int death_ticks = 136; int respawn_ticks = 136;
                    bool campaign = true; int lives = -1; };

class GameFlow {
  void configure(const FlowConfig&);                  // кламп + заметка
  bool configure(const FlowConfig&, std::string*);    // отказ с текстом
  void on_death();                                    // player_killed 0x4536e0
  void tick();                                        // один тик 1/91 с
  PlayerFlow state() const;
  int  ticks_left() const;
  bool respawn_due() const;                           // ровно один тик
  void on_respawn();                                  // 0x452670
  bool prompt_restart();                              // 0x443af9
  void restart();                                     // 0x4428f0(0/1)
  void return_to_hq();                                // Restart level from HQ
  static std::string_view state_name(PlayerFlow) noexcept;
  const FlowConfig& config() const;
  int  lives() const;            // -1 = без лимита
  int  deaths() const;           // принятые on_death()
  int  respawns() const;         // успешные on_respawn()
  int  restarts() const;         // restart()
  int  hq_returns() const;       // return_to_hq()
  std::uint64_t ticks_elapsed() const;
  const std::string& last_error() const;
  void clear_error();
};
```

`configure()` перезапускает автомат: состояние `alive`, таймеры и счётчики
обнулены, `lives()` = `config.lives`. Валидирующая форма отказывает при
`death_ticks < 0`, `respawn_ticks < 0`, `lives < -1`, не трогая ни настройки,
ни текущий автомат; обычная форма клампит (`<0 → 0`, `lives < -1 → -1`) и
пишет заметку в `last_error()`.

Порядок шага `tick()`: сброс однотикового импульса `respawn_due()` → таймер
текущего состояния → переход. Точная арифметика (по умолчанию, смерть на
тике 0):

| тик | `state()` | `ticks_left()` | `respawn_due()` |
|---:|---|---:|---:|
| 0 (`on_death`) | `dying` | 136 | 0 |
| 1..136 | `dying` | 135..0 | 0 |
| 137 | `dead` | 0 | **1** |
| 137 (`on_respawn`) | `respawning` | 136 | 0 |
| 138..272 | `respawning` | 135..1 | 0 |
| 273 | `alive` | 0 | 0 |

То есть: 136 тиков `dying`, на 137-м — `respawn_due`, окно `respawning` —
136 тиков (138..273), `alive` после 273-го. При `lives`, дошедших до 0,
тик 137 даёт `restart_prompt` (без `respawn_due`). `dead` сам не выходит:
выход — `on_respawn()` (в оригинале `0x452670` вызывается на тике порога) или
`prompt_restart()`.

`tick()` в `alive`/`dead`/`restart_prompt` не меняет состояние, но считает
тики. Повторный `on_death()` в `dying` — no-op с текстом (гейт `0x453700`
возвращается при `action_state == 7`), таймер не сбрасывается.

## 4. Числа теста (`./build/engine/tests/gm_game_flow_tests`)

```
game_flow: constants: death_ticks=136 respawn_ticks=136 enum alive=0 dying=1 dead=2 respawning=3 restart_prompt=4
game_flow: death              tick=  0 state=dying          left=136 due=0 deaths=1 respawns=0 lives=-1
game_flow: after tick         tick=  1 state=dying          left=135 due=0 deaths=1 respawns=0 lives=-1
game_flow: after tick         tick=136 state=dying          left=  0 due=0 deaths=1 respawns=0 lives=-1
game_flow: after tick         tick=137 state=dead           left=  0 due=1 deaths=1 respawns=0 lives=-1
game_flow: on_respawn         tick=137 state=respawning     left=136 due=0 deaths=1 respawns=1 lives=-1
game_flow: after tick         tick=272 state=respawning     left=  1 due=0 deaths=1 respawns=1 lives=-1
game_flow: after tick         tick=273 state=alive          left=  0 due=0 deaths=1 respawns=1 lives=-1
game_flow: due pulse: count=1 first=137 alive_tick=273 over 400 ticks
game_flow: due pulse: second death count=1 alive_tick=+273 deaths=2 respawns=2
game_flow: lives: first death lives=1 deaths=1
game_flow: lives: second death due=0 restart_prompt_tick=137 state=restart_prompt left=0 lives=0
game_flow: lives: restart -> state=alive lives=2 restarts=1 left=0 due=0
game_flow: lives=0: first death state=restart_prompt lives=0 due=0
game_flow: campaign=0 lives=1: after death state=alive lives=1
game_flow: prompt_restart(alive)=0 error="game_flow: prompt_restart() needs dying/dead (0x443af9), state alive"
game_flow: prompt_restart(dying left=126)=1 state=restart_prompt left=0 due=0
game_flow: prompt_restart: 50 ticks -> state=restart_prompt ticks_elapsed=60
game_flow: prompt_restart(dead)=1 state=restart_prompt due=0
game_flow: prompt_restart(respawning)=0 error="game_flow: prompt_restart() needs dying/dead (0x443af9), state respawning"
game_flow: return_to_hq(prompt) -> state=alive lives=1 left=0 due=0 hq_returns=1 restarts=0
game_flow: return_to_hq(respawning) -> state=alive left=0 due=0 lives=-1 hq_returns=1
game_flow: configure(death_ticks=-1)=0 error="game_flow: death_ticks must be >= 0 (GAME_OVER.md 2.1)"
game_flow: configure(respawn_ticks=-1)=0 error="game_flow: respawn_ticks must be >= 0 (model, GAME_OVER.md 2.1)"
game_flow: configure(lives=-2)=0 error="game_flow: lives must be -1 (unlimited) or >= 0 (model, GAME_OVER.md 2.2)"
game_flow: configure(clamp) death_ticks=0 respawn_ticks=0 lives=-1 error="game_flow: death_ticks -5 clamped to 0 (GAME_OVER.md 2.1); game_flow: respawn_ticks -1 clamped to 0 (model); game_flow: lives -3 clamped to -1, unlimited (model); "
game_flow: on_death twice: left 86 -> 86 deaths=1 error="game_flow: already dying, death ignored (player_killed 0x453700)"
game_flow: on_death(dead) error="game_flow: death ignored in state dead, restart()/return_to_hq() first"
game_flow: on_respawn(alive) error="game_flow: on_respawn() outside dead (state alive), ignored"
game_flow: on_death(restart_prompt) error="game_flow: death ignored in state restart_prompt, restart()/return_to_hq() first" state=restart_prompt
game_flow: 100 ticks in restart_prompt: state=restart_prompt left=0 deaths=1 elapsed=237
game_flow: death_ticks=0: after 1 tick state=dead due=1
game_flow: respawn_ticks=0: after on_respawn + 1 tick state=alive
game_flow: death_ticks=1: due after tick 2 = 1
game_flow: determinism: len=3504 hash1=5ea7d56985d1a5b9 hash2=5ea7d56985d1a5b9 equal=1
game_flow: ok (170 checks)
```

Проверено тестом:

1. точные тики (таблица §3) и `ticks_elapsed() == 273` после первого цикла;
2. `respawn_due()` — ровно один тик (счётчик по 400 тикам: `count=1`,
   `first=137`), повторная смерть даёт те же числа (273);
3. `lives = 2`: первая смерть тратит жизнь и респавнит, вторая уводит в
   `restart_prompt` ровно на 137-м тике без `respawn_due`; `restart()` →
   `alive`, `lives = 2`, таймеры 0; `lives = 0` с самого начала — то же на
   первой смерти; `campaign = false` игнорирует бюджет;
4. `prompt_restart()` из `dying`/`dead` → `restart_prompt` со сбросом
   таймеров, из `alive`/`respawning`/`restart_prompt` — отказ с текстом;
5. `return_to_hq()` из `restart_prompt` и из середины `respawning` → `alive`,
   таймеры 0, `lives` восстановлены, `hq_returns` растёт, `restarts` — нет;
6. негативы: `death_ticks = -1`, `respawn_ticks = -1`, `lives = -2` — отказ с
   текстом, прежние настройки и текущий автомат целы; кламп
   (`-5/-1/-3 → 0/0/-1`) с заметкой; повторный `on_death()` в `dying` не
   сбрасывает таймер (86 → 86) и не считает смерть;
   `on_death()` в `dead`/`restart_prompt`, `on_respawn()` вне `dead` — отказ;
   `tick()` в `restart_prompt` 100 раз меняет только `ticks_elapsed`;
7. границы: `death_ticks = 0` → `respawn_due` на 1-м тике; `death_ticks = 1` →
   на 2-м; `respawn_ticks = 0` → `on_respawn()` + 1 тик → `alive`;
8. детерминизм: одинаковый сценарий (2 смерти, 500 тиков) даёт одинаковую
   трассу `state/left/due` и FNV-1a 64 `5ea7d56985d1a5b9`.

## 5. Ограничения

- **Не подключено** к `GameRuntime`, `main.cpp`, меню и фронтенду: никто не
  вызывает `on_death()`/`tick()`, меню `RESTART GAME` (state 6) не
  рисуется, пункты меню не обрабатываются.
- **Нет анимации/трупа/звука смерти.** Отдельного «похоронного» джингла в
  оригинале нет (`GAME_OVER.md` §2.3); `PRESS ACTIVATE TO RESTART`
  (`0x4bc2c8`) — HUD-панель, вне модуля.
- **Нет сейва/автосейва** (§2.2) и **нет загрузки уровня**: `restart()`
  только меняет состояние.
- **Нет выбора точки респавна:** `0x452670` (кампания — `SET_PLAYER_START`,
  DM — самый далёкий слот, `DM_RULES.md` §4.2) не моделируется; в DM-модуле
  (`dm_state.h`) свой счётчик респавна.
- **Нет снимков штаба/миссии, `mission_counter`, денег:** пункты
  `0x4428f0(0/1)` копируют `0x3510` байт игрока и декрементируют счётчик
  миссий (§4.2) — это вне модуля.
- **Нет Esc-из-уровня** (`0x48b54c`), титров, экрана конца уровня (опкод 91) и
  цепочки «миссия → штаб → следующая миссия» (§4.4/§5): это отдельные срезы.
- **Нет гейтов Activate** (`[0x83d620]`, `[0x83d940]`, чит `regen`): в
  оригинале они блокируют открытие меню (§2.1/§7.3), здесь `prompt_restart()`
  вызывающий решает сам.
- Тик фиксированный (1/91 с); 136 тиков ≈ 1.49 с — замер частоты, в коде
  модуля частота не зашита (как и в `dm_state`).

## 6. Связь с другими модулями

- `engine/src/game/dm_state.h` (`M9.2`) — правила DM: фраги, команды,
  гандикап и свой 136-тиковый таймер респавна. `game_flow` его не заменяет и
  не дублирует: здесь — поток игрока во времени, там — счёт матча.
- `engine/src/game/sim_player.h` (`M6.5.1`) — `alive()`/`respawn()` одного
  игрока в рантайме; `GameFlow` — надстройка над этим жизненным циклом, но
  пока не подключена.
- `docs/GAME_OVER.md` — источник фактов (адреса, строки, ассеты);
  `docs/DM_STATE.md` — стиль модуля и та же оговорка о направлении таймера.
