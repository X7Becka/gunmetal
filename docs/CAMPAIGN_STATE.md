# Прогресс кампании в движке (`M9.1`, срез 2)

**Статус:** модуль `engine/src/game/campaign_state.{h,cpp}` + тест
`campaign_state` (`engine/tests/test_campaign_state.cpp`, CTest label `quick`,
**258 проверок**). Чистое состояние: счётчик миссий, таблица переходов,
рестарт из штаба, цели/секреты и `GUNMETAL.HI`. Без `GameRuntime`, VM, меню,
сейв-писателя, часов, RNG и рендера; единственный ввод-вывод —
`load_hi()`/`save_hi()`.

Канон поведения — `docs/CAMPAIGN_FLOW.md` (снят с `gundx.exe` v1.36 только на
чтение) и `tools/re/campaign_flow.py`; здесь — реализация и её числа.

Проверка:

```sh
cmake --build build --target gm_campaign_state_tests -j 4
./build/engine/tests/gm_campaign_state_tests     # campaign_state: ok (258 checks) ...
ctest --test-dir build -R '^campaign_state$' --output-on-failure
python3 regress.py --mirror --json
```

## 1. Что доказано (код игры и данные поставки)

| факт | значение | источник |
|---|---|---|
| поле счётчика | `ds:0x80f9cc`, u32, **номер следующей** миссии | `CAMPAIGN_FLOW.md` §2 |
| новая игра | `counter = 1`, имя `LEVEL0` | §2: `0x442a0e`, `0x442a4c`, `0x442cf7`; `0x442a39`..`0x442a4c` (`0x4ab6d4`) |
| `NEXT_LEVEL` | `sprintf("LEVEL%ld", counter)`, затем `counter++` | §2/§3: `0x457b11`, `0x457b17`..`0x457b21` (fmt `0x4b8588`), `0x457b26`..`0x457b2f` |
| `NEW_LEVEL "stem"` | копия ствола, счётчик **не** меняется | §3: `0x457af3`, NUL в `0x5a4817` |
| `NEXT_LEVEL_NUMBER n` | `counter == n` | §2: cid 8, `0x45ea26` |
| рестарт из штаба | `counter -= 1`, кламп `>= 1`; гейт `param & 1` и имя не `LEVEL0` | §2: `0x4428f0(1)` -> `0x44295e`, `0x442973`, `0x442952`; меню `0x4bd3fc`/`0x48da09`, скрыто для `LEVEL20`/`SECRT1`/`SECRT2` (`0x4888e4`) |
| загрузка сейва | счётчик берётся как есть, декремента нет | §2: `0x45586e`; поправка к `FORMATS.md` §15 — §11 |
| миссия -> штаб | `NEW_LEVEL "<hq>"` в `entity -8`, затем `NEXT_LEVEL` у штаба (`entity 305`) | §1/§3 |
| сайты переходов | **66** = 60 `NEW_LEVEL` + 6 `NEXT_LEVEL` на 36 уровнях | §3, `--summary`; `docs/VM_SCENE.md` §4 |
| 6 `NEXT_LEVEL` | 5 штабов + `level19` (единственная миссия без штаба) | §3 |
| уровни | 24 миссии `level1`..`level24`, 5 штабов (`level0`, `level00`, `level000`, `level0a`; `level0b` = COMMAND POST), 4 DM, 3 секретных | §3/§8 |
| `level6` | два под-скрипта `-8`, допустимы **оба** штаба: `level00` (`0xdad9`) и `level0a` (`0xdc02`) | §3 |
| `level4`/`level16` | секретные ветки `secrt1` (`0xde21`)/`secrt2` (`0xe44d`) номер миссии не меняют | §3 |
| `level19` | `-8` = `NEXT_LEVEL` (`0xf5a0`) при `counter = 20`, сразу `LEVEL20` | §2/§3 |
| `level24` | `-8` (`0xf799`) только `IF DEATHMATCH_GAME { NEW_LEVEL "DM1" }`, кампанийного выхода нет | §7 |
| `END_GAME` | 0 сайтов в поставке | §3/§7 |
| счётчик при миссии `m` | `m + 1`; у `level24` остаётся 25 | §2 (проверено на 24/24) |
| цели | 25 уровней объявляют, 51 первичная строка, 35 объявлений, 48 сайтов `LEVEL_OBJECTIVES_COMPLETE`, максимум 30 на уровень | §4: `0x45bdf6` (`cmp eax,0x1e`), `0x45e9f0` |
| цели — маска | биты `0x80f9d0`, `OR_LEVEL_OBJECTIVES_BITS` (104) | §4: `0x45be40`, сброс `0x4570a3` |
| секреты | 101 объявлено (`SET_LEVEL_SECRETS` 94 -> `0x80d8c0`), 105 сайтов инкремента (95, `0x45b978` -> `0x4536b0`) | §5 |
| счётчик секретов игрока | `player+0x33cc` (`0x81c40c`), сброс на старте уровня | §5: `0x45280f` |
| `GUNMETAL.HI` | 576 байт = 9 записей `i32 seconds; char name[60]`; сравнение имён регистронезависимо, хранится минимум; дубликаты законны (дописывание) | §6: `0x45c880`, `0x40dc70`, `0x416850`, `0x45c8d6` |

## 2. Что модель (не доказано кодом)

Каждое место помечено `MODEL` в `campaign_state.h`/`.cpp`.

1. **`new_level()` = инкремент.** Имя взято из задания; численно это
   `NEXT_LEVEL` (`0x457b2e`). Оригинал инкрементирует без ограничений, модель
   отказывается уходить за поставку: счётчик живёт в `1..25` (24 миссии,
   `level24` играется при 25), 25-й инкремент — `false` с текстом. Команда
   `NEW_LEVEL` счётчик не меняет и представлена только таблицей.
2. **`next_level(level)` — один шаг кампании.** Миссия отдаёт свой штаб,
   штаб/`level19` — `level<counter>` с инкрементом. Какую из нескольких ветвей
   `NEW_LEVEL` берёт игра (ветки `DEATHMATCH_GAME`, секретные ветки
   `level4`/`level16`, два штаба у `level6`), не моделируется: в таблице
   кампанийная ветка, у `level6` — `level00`, альтернатива `level0a`.
3. **Стволы в нижнем регистре** (`level2`): оригинал собирает буфер `LEVEL<n>`,
   а `load_level` (`0x442ef0`) разрешает файл; имена файлов в поставке
   строчные.
4. **`load(counter)` клампит `>= 1`.** Оригинал читает поле сейва как есть
   (`0x45586e`), нижнюю границу ставит только путь рестарта (`0x442973`).
   Верхней границы нет — значение сохраняется как есть.
5. **`CampaignStats`** — учёт этого модуля: в оригинале таких полей нет
   (номер миссии живёт только в счётчике). `hq_visits` считает исполнения
   `NEXT_LEVEL` у штабов (за кампанию 23), `restarts` — вызовы меню (включая
   заклампленный на 1), `missions`/`objectives_done` — события вызывающего.
6. **`on_objective()` считает цели.** Оригинал держит битовую маску
   `0x80f9d0` (повторная установка бита идемпотентна); ни биты, ни индексы
   целей не моделируются.
7. **`add_secret()` без клампа и без дедупликации.** `0x4536b0` инкрементирует
   счётчик на каждом сайте. Данные доказывают, что «найдено» может превысить
   объявленное: `level3` — 8 сайтов при 6 объявленных, `secrt2` — 2 при 0,
   `temple` — 1 при 0; правило «не более одного раза на секрет» **не**
   доказано (`level15`: 4 сайта на 5 объявленных), поэтому его нет.
8. **Имя записи `.hi` — C-строка до первого NUL.** Оригинал сравнивает и
   печатает её как строку (`0x416850`), в поставке после терминатора везде
   нули (9/9), поэтому round-trip побайтовый. Файл с мусором после NUL
   сохранился бы без этого мусора.
9. **Больше 9 записей — отказ.** Модель держит ровно поставочную таблицу
   (`std::array<int, 9>`); игра дописывает файл без предела.
10. **`load()` сбрасывает счётчики модуля и `next_stem()`.** Имя текущего
    уровня берётся из шапки сейва, а не из счётчика, поэтому до следующего
    шага переход не выводится.

## 3. API (`game/campaign_state.h`)

```cpp
struct CampaignStats { int missions, hq_visits, secrets, objectives_done, restarts; };
enum class CampaignRole : uint8_t { hq, mission, deathmatch, secret };
enum class CampaignExit : uint8_t { next_level, new_level, campaign_end, other };
struct CampaignLevel {
    string_view stem, name; CampaignRole role;
    int new_level_sites, next_level_sites; CampaignExit exit;
    string_view target, campaign_alt;
};
std::span<const CampaignLevel> campaign_levels() noexcept;      // 36 строк
const CampaignLevel* campaign_level(string_view stem) noexcept; // без регистра

class CampaignState {
  public:
    void new_game();                 // counter = 1, next_stem = "level0"
    void load(int counter);          // как в сейве, кламп >= 1, счётчики сброшены
    int counter() const;
    int mission_index() const;       // counter - 1 (MODEL)
    bool new_level();                // 0x457b2e; false на 25
    bool next_level(const string& level); // шаг по таблице; false + текст
    const string& next_stem() const;
    void restart_level_from_hq();    // 0x44295e, кламп 0x442973
    void on_mission_complete(), add_secret(), reset_level_secrets(), on_objective();
    int secrets() const, objectives_done() const;
    const CampaignStats& stats() const;
    bool at_last_mission() const;    // counter == 25 (level24)

    std::array<int, 9> best_times; std::array<string, 9> best_names;
    int hi_count() const;
    bool load_hi(const string& path, string* error = nullptr);
    bool save_hi(const string& path, string* error = nullptr) const;
    bool set_hi_record(int index, int seconds, const string& name, string* error = nullptr);
    bool set_hi_time(int index, int seconds, string* error = nullptr);
    int best_time(string_view name) const;  // минимум по имени, -1 если нет
    const string& last_error() const;
};
```

Константы: `kCampaignMissions` 24, `kCampaignHqs` 5, `kCampaignLevelCount` 36,
`kCampaignTransitionSites` 66 (`kCampaignNewLevelSites` 60 +
`kCampaignNextLevelSites` 6), `kCampaignFirstCounter` 1,
`kCampaignLastCounter` 25, цели 25/51/35/48 и `kCampaignMaxObjectives` 30,
секреты 101/105 и `kCampaignSecretOffset` `0x33cc`, `kHiRecordSize` `0x40`,
`kHiNameSize` 60, `kHiRecords` 9, `kHiFileSize` 576.

## 4. Числа теста (`campaign_state`, 258 проверок)

Счётчик и инкременты:

```
new game: counter=1 next=level0 mission=0 last=0
load: 0->1 (-5)->1 7->7 25->25
increments: 24 steps 1->25 monotonic=1 25th=0 counter=25
```

Таблица переходов (сумма сайтов — независимая сверка §3/§8):

```
table: levels=36 missions=24 hqs=5 dm=4 secret=3 \
       new_level_sites=60 next_level_sites=6 total=66
NEXT_LEVEL exceptions (6): level0(HEADQUARTERS), level00(HEADQUARTERS), \
       level000(HEADQUARTERS), level0a(HEADQUARTERS), level0b(COMMAND POST), level19(WASTELAND)
rows: level6=CADTLELTL sites=4 hq=level00 alt=level0a | level4=3 level16=3 | \
       level19=WASTELAND next=1 | level24=KELLER CORE exit=end
```

Полный проход кампании (штабы раскрываются счётчиком):

```
walk: missions=24 increments=24 counter=25 hq_visits=23
hq visits: level0=1 level00=8 level000=8 level0a=2 level0b=4 total=23
order=1 acts=1 counter=1
```

`order` — миссии идут `level1`..`level24` без пропусков, `acts` — каждая
миссия возвращается в штаб своего акта (`level00` 1–8, `level000` 9–16,
`level0a` 17–18, `level0b` 20–23, `level19` сразу в `level20`), `counter` —
`counter == m+1` во время миссии `m`.

Рестарт, цели, секреты:

```
restart: 2->1, 1->1 (clamp 0x442973), 25->24
objectives: levels=25 lines=51 declarations=35 complete_sites=48 max_per_level=30
secrets: declared=101 sites=105 offset=0x33cc
tally: level3 6 declared/8 sites -> 8, reset -> 0, secrt2 0 declared/2 sites -> 2
```

`GUNMETAL.HI` (реальный файл установки, только чтение):

```
hi: file /mnt/d/games/Gunmetal 3dfx/GUNMETAL.HI bytes=576 records=9 fnv=0x80bf2f264bebe680
hi: records=9 times=146,1570,18,109,18,7,11,40,6
hi best: TESTING RANGE=18 lowercase=18 KELLER CORE=1570 NATAKA TOWER=7 missing=-1
hi round-trip: written=576 identical=1
hi edit: record=2 18->5 differing_bytes=1 first=0x80 last=0x80
hi reread: records=9 times=146,1570,5,109,18,7,11,40,6 TESTING RANGE=5
hi rename: differing_bytes=9 inside_record8=1
```

`fnv` печатается как отпечаток установки и не проверяется (файл — данные
игрока, игра его дописывает). Отказы `.hi` (все с текстом, таблица цела):
нет файла, 3 байта, 10 записей; индекс `-1`/`9`, время `-1`, имя пустое/61
байт/с NUL, недоступный путь записи.

Негативы переходов: `nosuchlevel` -> `unknown level`, `dm1`/`secrt1` -> `not on
the campaign path`, `level24` -> `no campaign exit`, штаб при `counter = 25` ->
`LEVEL25` не существует. Временные файлы пишутся в `/var/tmp`
(`GM_CAMPAIGN_TMPDIR` переопределяет) и удаляются.

## 5. Ограничения

- **Не подключено** к `GameRuntime`, `main.cpp`, фронтенду, меню паузы,
  сейв-писателю и CLI: ни `new_game()`, ни рестарт, ни `GUNMETAL.HI` не
  вызываются движком. Это чистый модуль + тест.
- Нет текстов целей и брифингов (`0x45be11`, `SML\BRIEF\LEVEL%ld.SML`), нет
  отрисовки `SECRETS FOUND`/`BEST TIME`/`TIME` — только числа.
- Нет DM/секретных переходов как кампанийных: `dm1`..`dm4`, `secrt1`,
  `secrt2`, `temple` помечены `CampaignExit::other`; их 20 сайтов входят в
  сумму 60, но не в цепочку.
- Выбор из нескольких `NEW_LEVEL`-ветвей и двух штабов `level6` не
  моделируется (см. §2.2).
- Биты целей не восстанавливаются (сейв-блок 11 через `entity -3`,
  `CAMPAIGN_FLOW.md` §4) — модуль не читает и не пишет сейвы.
- `level24` не завершает кампанию флагом: `next_level("level24")` просто
  возвращает `false` с текстом; признака «кампания пройдена» в оригинале нет
  (§7: титры — отдельный путь меню).
