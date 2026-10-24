# Deathmatch state in the engine (`M9.2`, slice 2)

**Статус:** модуль `engine/src/game/dm_state.{h,cpp}` + тест `dm_state`
(`engine/tests/test_dm_state.cpp`, CTest label `quick`, 183 проверки).
Чистое состояние матча: без `GameRuntime`, VM, сейвов, часов, RNG, сети и
рендера. Правила — из `docs/DM_RULES.md` (сняты с `gundx.exe` только на
чтение); эталонная модель — `tools/re/dm_rules.py`.

Проверка:

```sh
cmake --build build --target gm_dm_state_tests -j 4
./build/engine/tests/gm_dm_state_tests        # dm_state: ok (183 checks)
ctest --test-dir build -R '^dm_state$' --output-on-failure
```

## 1. Что доказано (код игры)

| факт | значение | источник |
|---|---|---|
| `game_mode` | `ds:0x83d2c8`: `0` кампания, `1` normal DM, `2` classic DM, `3` hunt | `DM_RULES.md` §2.1; `0x4421c0`, `0x45eb1c`, `0x45eb06`, `0x4421d0` |
| `mp_flags` `team` | `0x01`, `/team` | §2.2, `0x44a752` |
| `mp_flags` `ctf` | `0x02`, `/flag` | §2.2, `0x44a777` |
| `mp_flags` `no_monsters` | `0x04`, `/nomonsters` | §2.2, `0x44a79c` |
| `mp_flags` `freeze` | `0x08`, `/freeze` | §2.2, `0x44a7c1` |
| `mp_flags` `no_buy` | `0x10`, `/nobuyammo` | §2.2, `0x44a80b` |
| `mp_flags` `handicap` | `0x20`, `/handicap` | §2.2, `0x44a7e6` |
| слоты игроков | 8 на карту | §4.1 |
| убийство | `frags[killer] += 1` | §3.2, `0x453812` |
| фраги жертвы при обычном убийстве | **не меняются** | §3.2: ветка `-1` (`0x453875`) входит только при невалидном/своём убийце (`0x4537af`) |
| team kill | `frags[killer] -= 1` и только если у **жертвы** `frags > 0`; клампа у убийцы нет | §3.2, `0x4537f5`/`0x4537fd` |
| самоубийство / смерть от мира | `frags[victim] -= 1`, только если было `> 0` | §3.2, `0x453875`/`0x45387f` |
| смерть | `action_state = 7`, `+0x33d4 = 0` | §4.3, `0x453763`/`0x45372f` |
| респавн | `+0x33d4 >= 0x88` = **136** тиков | §4.3, `0x443b0a` |
| лимит фрагов | **нет** (ни одной проверки «фраги >= N») | §3.4 |
| лимит времени DM | **нет**; `ds:0x80d898` ставят только `level15` = 38220 и `level23` = 13650 | §3.4 |
| гандикап | `diff = max_other - mine`; `diff > 50/30/25/20/15/10/7/4` → 5x/4x/3x/2x/1x/½/¼/⅒ стартовых кредитов | §5, `0x452e9a`..`0x452f71` |
| командная проверка (`is_friendly`) | гейт `mp_flags & 0x1`, команда — `player+0x344c` | §3.2/§6 |

## 2. Что модель (не доказано кодом)

Каждое место помечено `MODEL` в `dm_state.h`/`dm_state.cpp`.

1. **`deaths`/`score`.** В оригинале одно i32-поле счёта `+0x3270`
   («отдельного поля очков нет», §5). `score` — зеркало `frags`; `deaths`
   считает смерти в этом модуле и в игре не существует.
2. **`frag_limit`/`time_limit_ticks`.** В поставочном DM лимитов нет (§3.4),
   поэтому оба — opt-in правила движка, `0` = без лимита. Направление
   счётчика (вниз) взято у `ds:0x80d898` (`0x457230` вычитает дельту), но ни
   одна DM-карта его не ставит.
3. **Направление таймера респавна.** В игре `+0x33d4` растёт до `0x88`; здесь
   `respawn_ticks` убывает. Доказана только величина по умолчанию (136).
4. **CTF-дружелюбность.** Ветка team kill в коде проверяет только бит `0x1`
   (§3.2); применять то же к `ctf` (`0x2`) — модель.
5. **`handicap` — вход вызывающего.** Доказанная лестница вынесена в
   `dm_handicap_percent(frag_diff)`; денежная часть (`cash = salvage +
   [0x83d310] + бонус`, §4.4/§5) не моделируется — в `DmPlayer` нет денег.
6. **Ничья.** Победитель — игрок с максимумом фрагов; равный максимум → `-1`.
   В оригинале DM не заканчивается, а таблица результатов лишь сортируется по
   фрагам (`0x486afc`, §3.3), тай-брейк матча не определён.
7. **Смерть при `respawn_delay_ticks == 0`** — мгновенный респавн (в игре
   `0x88` не настраивается).
8. **Матрица `kills_by_victim[8]`** (§3.1) и текст убийства (`0x4936a0`) не
   моделируются.

Событие `-7 SYSTEM_PLAYER_KILLED` (§3.2, `docs/EVENTS.md` §4.1) **не**
постится: оригинал считает фраг прямо в `player_killed`, событие — для
скриптов/эффектов.

## 3. API (`game/dm_state.h`)

```cpp
enum class DmMode { campaign=0, normal=1, classic=2, hunt=3 };
enum class DmFlag : std::uint32_t {
    team=0x01, ctf=0x02, no_monsters=0x04, freeze=0x08, no_buy=0x10, handicap=0x20 };
inline constexpr std::uint32_t kDmKnownFlags = 0x3F;
inline constexpr int kDmMaxPlayers = 8;
inline constexpr int kDmRespawnTicks = 136;
int dm_handicap_percent(int frag_diff) noexcept;

struct DmSettings { DmMode mode; std::uint32_t flags; int frag_limit;
                    int time_limit_ticks; int respawn_delay_ticks; int handicap; };
struct DmPlayer   { int frags; int deaths; int score; int respawn_ticks;
                    bool alive; int team; };

class DmState {
  void configure(const DmSettings&);                  // кламп + заметка
  bool configure(const DmSettings&, std::string*);    // отказ с текстом
  int  add_player(int team = 0);                      // -1 при 8 слотах
  int  player_count() const;
  void on_kill(int victim, int killer);
  void tick(std::uint64_t ticks = 1);
  bool finished() const;
  int  respawn_left(int player) const;                // -1 вне диапазона
  bool is_friendly(int a, int b) const;
  int  winner() const;                                // -1: ничья/не конец
  static std::string_view mode_name(DmMode) noexcept;
  const DmSettings& settings() const;
  int  time_left() const;
  std::uint64_t ticks_elapsed() const;
  const DmPlayer* player(int index) const;            // nullptr вне диапазона
  std::span<const DmPlayer> players() const;
  const std::string& last_error() const;
  void clear_error();
};
```

`configure()` перезапускает матч: игроки и часы сбрасываются, счётчики
обнуляются. Порядок шага `tick()`: часы матча → таймеры респавна (0 →
`alive`), затем `time_left`. После `finished()` часы стоят, `on_kill()`
отклоняется с текстом.

## 4. Числа теста (`./build/engine/tests/gm_dm_state_tests`)

```
modes: campaign=0 normal=1 classic=2 hunt=3
flags: team=0x01 ctf=0x02 no_monsters=0x04 freeze=0x08 no_buy=0x10 handicap=0x20 known=0x3f
respawn=136 max_players=8
friendly(team): (0,1)=1 (0,2)=0 (2,3)=1 (0,0)=0 (0,99)=0
friendly(ctf):  (0,1)=1 (0,2)=0
kill: killer_frags=1 victim_frags=0 victim_deaths=1 victim_respawn=136
      victim_alive=0 respawn_left(0)=0 respawn_left(1)=136
after 1 tick:   left=135 alive=0 elapsed=1
after 136 ticks: left=0 alive=1 elapsed=136
kill on dead victim: ignored, error="dm: victim 1 is already dead, ..."
self-kill at 0 frags: victim keeps 0 (guard 0x453875), deaths=1
world kill (killer=99): victim with 0 frags stays 0, killer untouched
team kill (killer 2, victim 0 with 2 frags): victim stays 2, killer 2 -> 1
third team kill: killer 2 -> -1 (no clamp in the original)
frag limit 3: after 2 kills finished=0 winner=-1; 3rd kill frags=3
              finished=1 winner=0 score=3
draw (time limit 1, 1:1): finished=1 winner=-1
time limit 5: after 4 ticks left=1 finished=0; after 5 left=0 finished=1 winner=0
time limit 0: after 1000 ticks finished=0 left=0
handicap: 51->500 50->400 31->400 30->300 26->300 25->200 21->200 20->100
          16->100 15->50 11->50 10->25 8->25 7->10 5->10 4->0 0->0 -3->0
          (18 точек, 0 mismatches — совпадает с tools/re/dm_rules.py)
configure(frag_limit=-1): accepted=0, текст, старые настройки целы
clamped: mode 7->0, flags 0xff->0x3f, frag/time/respawn/handicap <0 -> 0
slots: count=8 ninth=-1 error="dm: no free player slot (8 slots, ...)"
victim out of range: frags=0/0 error="dm: victim 2 outside 0..1, kill ignored"
итог: dm_state: ok (183 checks)
```

## 5. Ограничения

- **Не подключено** к `GameRuntime`, `main.cpp`, сети и ботам. Ботов в игре
  нет (`DM_RULES.md` §6), слоты заполняет только `player_join` `0x453120`.
- **Нет спавн-точек:** выбор слота `SET_DEATHMATCH_START` (8 на карту, 248
  сайтов, §4.1/§4.2) и «самый далёкий от живых» респавн не моделируются.
- **Нет оружия/вещей по флагам:** `no_buy`, `freeze`, `no_monsters` и
  hunt-арсенал `0x7fffff` (§2.3/§2.4/§7) только хранятся как биты.
- **Нет денег:** гандикап — только лестница процентов, `cash`/`salvage`
  (§4.4/§5) вне модуля.
- **Нет `kills_by_victim`, текста убийства, HUD, таблицы результатов.**
- **Нет DM-цепочки уровней** (`dm1 -> ... -> level24 -> dm1`, §3.4) и
  лимитов оригинала: `frag_limit`/`time_limit_ticks` — правила движка.
- Тик — фиксированный (91 Гц, `docs/PLAYER_GHIDRA.md`); 136 тиков ≈ 1.49 с
  при этой частоте (`DM_RULES.md` §4.3), но частота в коде не зашита и в игре
  не измерялась (§10.1, там же оценка ≈2.3 с при 60 Гц), поэтому величина
  `respawn_delay_ticks` — вход вызывающего.
