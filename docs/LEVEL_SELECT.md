# Экран выбора уровня (`M8.4`/`M9.1`, срез 2)

**Статус:** модуль `engine/src/ui/level_select.{h,cpp}` + тест `level_select`
(`engine/tests/test_level_select.cpp`, CTest label `quick`, **407 проверок**).
Чистое состояние: список 36 уровней, разблокировка по счётчику кампании,
лучшие времена из `Records`, счётчики секретов, курсор и события
`launch`/`locked`/`back`. Ни пикселей, ни шрифтов, ни чтения файлов: данные
`GUNMETAL.HI` приходят через `Records`, прогресс — через `CampaignState`.

Канон поведения — `docs/UI_SCREENS.md` §2/§3.7/§10, `docs/FORMATS.md` §15 и
`docs/CAMPAIGN_FLOW.md` §1–§3/§8. Всё снято с `gundx.exe` v1.36 (только
чтение) и с поставки; Ghidra не использовалась — только `tools/re/exelib.py`
(дизассемблер + строки).

Проверка:

```sh
cmake --build build --target gm_level_select_tests -j 4
./build/engine/tests/gm_level_select_tests     # level_select: ok (407 checks) ...
ctest --test-dir build -R '^level_select$' --output-on-failure
python3 regress.py --mirror --json
```

## 1. Что доказано (код игры и данные поставки)

| факт | значение | источник |
|---|---|---|
| экран | id **17**, `SELECT LEVEL FOR NEW GAME`, обработчик `0x48a3c0` | `UI_SCREENS.md` §2; заголовок `0x4bd9e8` |
| шаг строки | 8 (`mov [0x662d58], 8`) | `0x48a3ce` |
| геометрия | x = 14, y = -(27 + 8·i); `ESC TO CANCEL` (`0x4bd9d8`), строка 111 | `0x48a481` (`push 0xe`), `0xffffffe5` = -27; `0xffffff91` = -111 |
| видимых строк | **10** (окно `i .. i+10` и кламп для стрелок) | `0x48a408` (`lea ecx,[ebx+0xa]`), `0x48a438` |
| число строк | runtime-счётчик `ds:0x68ddb0` -> `[0x67f10c]` | `0x48a3c9` |
| переходы в экран / из экрана | 21 -> 17 (`0x48dca6`), 17 -> 23 (`0x48e281`); Esc в §4 не описан | `UI_SCREENS.md` §4 |
| список — не таблица, а скан каталога | `0x48b170` -> `0x48b1c0`, маска `S?W` (`0x4bdca8`), шаблон `"%s\*.%s"` (`0x4bdccc`) | `FORMATS.md` §15 |
| что попадает в список | все `*.sbw` каталога игры; расширение срезается по первой `.` | `0x48b2ca`..`0x48b2f8` |
| хранение | массив `char*` `0x68cda8`, счётчик `0x68ddb0` | `0x48b2fc`..`0x48b306` |
| сортировка | `qsort`-подобный `0x49ff70` (вставки при <= 8 элементов, иначе быстрая сортировка с опорным средним элементом) с компаратором `0x48b350` | `0x48b19c`..`0x48b1a9`, `0x49ffc4`..`0x4a0032` |
| компаратор | два `strncmp(name,"LEVEL",5)` (`0x415ff0`, строка `0x4bdcd4`): имя на `LEVEL` — раньше; иначе меньшее `strlen` — раньше; равные длины -> 0 | `0x48b35b`/`0x48b375`, `0x48b387`..`0x48b39d` |
| `0x415ff0` побайтовый | **регистрозависимый** (нет складывания регистра), в отличие от `0x416850` (нижний регистр в `0x41687a`..) | дизассемблер |
| имя для показа | только при отрисовке: `0x48a4c0(stem)` = `sprintf(buf,"%s - ",stem)` (`0x4bda04`) + текст строки `LVLNAMES.SLU`, максимум 30 символов (`cmp ecx,0x1e`) | `0x48a4d9`..`0x48a560` |
| нет строки в `LVLNAMES.SLU` | показывается сам ствол (пять штабов, `temple`) | `0x48a53b` (возврат аргумента) |
| данные поставки | 36 `*.sbw`; `LVLNAMES.SLU` — **30** строк (`LEVEL1`..`LEVEL24`, `SECRT1`, `SECRT2`, `DM1`..`DM4`), без штабов и без `DM5`/`TEMPLE` | `ls`, файл 595 Б; `FORMATS.md` §15 |
| счётчик кампании | `ds:0x80f9cc` — номер **следующей** миссии; миссия `N` играется при `counter = N+1` | `CAMPAIGN_FLOW.md` §2 |
| состав 36 уровней | 24 миссии, 5 штабов (`level0`, `level00`, `level000`, `level0a`, `level0b` = COMMAND POST), 4 DM, 3 секретных | §8 |
| штабы по актам | 1..8 -> `level00`, 9..16 -> `level000`, 17..18 -> `level0a`, 20..23 -> `level0b`; `level0` — старт | §3 |
| `level19` | `WASTELAND`, единственная миссия без штаба (`NEXT_LEVEL` сама, `0xf5a0`) | §2/§3 |
| `level24` | `KELLER CORE`, кампанийного выхода нет | §7 |
| секретные ветки | `level4` -> `secrt1` (`0xde21`), `level16` -> `secrt2` (`0xe44d`), без условий | §3, `FORMATS.md` §15 |
| `temple` | `DM5 - TEMPLE`, патч 1.36; не на пути кампании, строки в `LVLNAMES.SLU` нет | `FORMATS.md` §15 |
| секреты | **101** объявлено `SET_LEVEL_SECRETS` на **25** уровнях, максимум **6** | `CAMPAIGN_FLOW.md` §5/§8 |
| `GUNMETAL.HI` в поставке | 146 `TESTING RANGE`, 1570 `KELLER CORE`, 18 `TESTING RANGE`, 109 `POWER STATION`, 18/7 `NATAKA TOWER`, 11 `OIL RIG`, 40 `OIL REFINERY`, 6 `CADTLELTL` | §6 |

## 2. Что модель (не доказано кодом)

* **Порядок `levels()`** — кампанийный: миссии 1..24, затем пять штабов, затем
  секретные, затем `dm1`..`dm4` (DM — отдельным блоком в конце). Такого порядка
  в оригинале нет: экран строит список сканом каталога и сортирует его
  компаратором `0x48b350` (§1). Воспроизвести скан здесь нельзя и не нужно
  (модуль не читает каталог), а порядок скана зависит от **регистра имён
  файлов**: `0x415ff0` сравнивает байты, поэтому на этой установке
  (`level1.sbw`, нижний регистр) ветка `LEVEL` не срабатывает вообще и работает
  только правило длины, а на FAT/CD-установке (`LEVEL1.SBW`) все `LEVEL*` идут
  первыми. При равных длинах компаратор возвращает 0, и `0x49ff70` оставляет
  относительный порядок неопределённым.
  Это уточнение к формулировке `FORMATS.md` §15/`UI_SCREENS.md` §10 («`LEVEL*`
  первыми, внутри — по длине»): она верна только для имён в верхнем регистре.
* **Порог разблокировки** (`unlocked = counter >= threshold`):

  | уровень | порог | почему |
  |---|---:|---|
  | миссия `N` | `N` | счётчик называет следующую миссию: `level1` -> 1, `level19` -> 19 |
  | `level0` | 1 | стартовый штаб новой игры (`0x442a0e`) |
  | `level00` | 9 | счётчик при игре `level8` (конец акта 1) |
  | `level000` | 17 | счётчик при игре `level16` (конец акта 2) |
  | `level0a` | 19 | счётчик при игре `level18` (конец акта 3) |
  | `level0b` | 24 | счётчик при игре `level23` (конец акта 4) |
  | `secrt1` | 5 | ворота в `level4`, счётчик при её игре |
  | `secrt2` | 17 | ворота в `level16` |
  | `temple`, `dm1`..`dm4` | 0 (всегда) | не на пути кампании; экран 17 пускает любой `*.sbw` |

  В оригинале блокировки нет вовсе — список это весь каталог. Порог
  означает «достижим при таком счётчике», а не «игрок там был»: счётчик не
  различает «последняя миссия акта идёт» и «она завершена» (значение одно и то
  же).
* **`completed`** — `mission < counter`, только для миссий. Что штаб, секрет
  или DM-карта посещены, игра не хранит (счётчик — всё состояние прогресса).
* **`best_seconds`** — минимум `Records` по имени уровня `SET_LEVEL_NAME`
  (не по стволу), `-1` без записи или без `Records`.
* **`secrets_found`** — tally `CampaignState::secrets()` только у миссии,
  которая играется (`mission == counter - 1`), у остальных 0: tally
  сбрасывается на старте каждого уровня (`0x45280f`).
* **`secrets_declared`** — числа `SET_LEVEL_SECRETS` из §8 (жёсткая таблица в
  `.cpp`; в `campaign_state` этих полей нет).
* **`name`** — ствол файла (`level1`, `level0b`, `dm3`): именно его даёт скан,
  его открывает `load_level` (`0x442ef0`) и его возвращает `launch`.
  Печатные названия (`SET_LEVEL_NAME` либо строка `LVLNAMES.SLU`) — дело
  отрисовки, в записи не хранятся. Взять их можно из
  `campaign_level(name)->name`.
* **`move()`** — кламп на краях, без зацикливания и **без пропуска
  заблокированных** строк: в оригинале блокировок нет, а курсор, прыгающий
  через строки, сделал бы ответ `locked` недостижимым.
* **`build()`** отказывает при `counter < 1` (текст в `last_error()`, список
  пуст) и больше ни при чём; `Records* == nullptr` допустим (все времена `-1`).
  Через нынешний `CampaignState` `counter < 1` недостижим: `new_game()` даёт 1,
  `load()` клампит `>= 1` (`0x442973`), так что отказ — защита для будущего
  вызывающего.

## 3. API (`ui/level_select.h`)

```cpp
enum class LevelKind { mission, headquarters, secret, deathmatch, unknown };
enum class LevelSelectEvent { none, launch, locked, back };

struct LevelEntry {
    std::string name;         // ствол: level1, level0b, dm3
    LevelKind kind;           // mission / headquarters / secret / deathmatch
    int mission;              // 1..24 у миссии, 0 иначе
    bool unlocked;            // counter >= порога (MODEL)
    bool completed;           // mission < counter (MODEL)
    int best_seconds;         // GUNMETAL.HI, -1 если нет
    int secrets_found;        // tally играемой миссии, иначе 0
    int secrets_declared;     // SET_LEVEL_SECRETS уровня, 0 если нет
};

class LevelSelect {
 public:
    static constexpr int kLevelCount = 36;   // CAMPAIGN_FLOW.md §8
    static constexpr int kVisibleRows = 10;  // 0x48a408

    void build(const CampaignState& campaign, const Records* records);
    const std::vector<LevelEntry>& levels() const;
    int selected() const;
    void move(int delta);
    bool activate(std::string* error = nullptr);   // launch | locked
    bool select(std::string_view stem, std::string* error = nullptr);
    void back();                                   // event = back
    LevelSelectEvent event() const;
    const std::string& launched() const;           // ствол последнего launch
    std::optional<LevelEntry> current() const;
    const std::string& last_error() const;

    static LevelKind kind_of(std::string_view stem);
    static int mission_of(std::string_view stem);
    static int declared_secrets(std::string_view stem);
    static int unlock_counter(std::string_view stem); // 0 = всегда, -1 = нет такого
};
```

Семантика событий: `build()`/`move()`/`select()` дают `none`; `activate()` на
доступной строке — `launch` и `launched()` = ствол; на заблокированной —
`locked` и текст «level level2 is locked: the campaign counter is 1, it opens
at 2»; пустой список — `false`, `none` и текст «no levels to select…»;
`back()` — `back` (у экрана 17 нет строки Esc в `UI_SCREENS.md` §4, поэтому
`back` — событие модели, а не переход).

## 4. Числа теста (`level_select`, 407 проверок)

```
$ ./build/engine/tests/gm_level_select_tests
level_select: list levels=36 missions=24 hq=5 secret=3 dm=4 unknown=0 declared=101 declaring=25 max=6 first=level1 last=dm4
level_select: counter=1 unlocked=7 completed=0 (start HQ, only mission 1)
level_select: counter=8 unlocked=15 completed=7 (campaign progress)
level_select: counter=17 unlocked=27 completed=16 (campaign progress)
level_select: counter=25 unlocked=36 completed=24 (campaign progress)
level_select: thresholds level1=1 level8=8 level9=9 level24=24 level0=1 level00=9 level000=17 level0a=19 level0b=24 secrt1=5 secrt2=17 temple=0 dm1=0
level_select: best level1=18 level2=109 level3=7 level4=11 level5=40 level6=6 level24=1570 level7=-1 level0=-1 with_time=7 hi=1
level_select: unknown level: error="unknown level nosuch"
level_select: empty list: error="no levels to select: the campaign state was refused (see build())"
level_select: secrets level4=3/5 current_level5=1 declared_total=101 declaring=25
level_select: determinism dump=650 chars unlocked=21 selected=0
level_select: ok (407 checks) levels=36 missions=24 hq=5 secret=3 dm=4 declared=101 first=level1 last=dm4 hi=1
```

Что стоит за числами:

* **список** — 36 строк ровно в кампанийном порядке; первые 24 —
  `level1`..`level24` с `mission == i+1`, затем `level0`, `level00`,
  `level000`, `level0a`, `level0b`, затем `secrt1`, `secrt2`, `temple`, затем
  `dm1`..`dm4`; первая строка `level1`, последняя `dm4`; имена уникальны и
  каждая строка сверена с `campaign_level()` (роль таблицы совпадает с
  `kind`);
* **секреты** — сумма `secrets_declared` = **101** на **25** уровнях, максимум
  **6** (`level3`, `level6`, `level12`, `level18`, `level21`), у
  `secrt2`/`temple` — 0 (нет `SET_LEVEL_SECRETS`);
* **разблокировка** — 7 / 15 / 27 / 36 доступных при счётчике 1 / 8 / 17 / 25
  и 0 / 7 / 16 / 24 завершённых миссий; границы актов проверены отдельно
  (`level00` открывается на 9, `level000` на 17, `level0a` на 19, `level0b` на
  24, `secrt1` на 5, `secrt2` на 17);
* **времена** — из настоящего `GUNMETAL.HI` (`hi=1`): `level1` 18 (минимум из
  146 и 18), `level2` 109, `level3` 7 (минимум из 18 и 7), `level4` 11,
  `level5` 40, `level6` 6, `level24` 1570; `level7`, `level8`, `level19`,
  штабы, секреты и DM — `-1`; записей с временем ровно **7**;
* **курсор** — `move(1)` с `level1` даёт `level2` (заблокированную, не
  пропускается), `activate` -> `locked` + текст; кламп на `dm4`/`level1`;
  `select("LEVEL2")` регистронезависим; `launch` отдаёт ствол `level1`;
* **негативы** — пустой (не собранный) список: `activate` -> `false` + текст;
  неизвестный ствол: `select("nosuch")` -> `false` + «unknown level nosuch»;
* **детерминизм** — два `build()` на одних данных дают идентичный дамп
  (650 символов), пересборка сбрасывает курсор и событие.

## 5. Ограничения

* **Не подключено** ни к фронтенду (`ui/frontend_menu.*`), ни к `GameRuntime`,
  ни к CLI: экран 17 нарисован не будет, `launch` никто не обрабатывает
  (`load_level` не вызывается).
* **Нет рендера**: ни миниатюр/превью уровней, ни рамки, ни стрелок, ни
  подсветки — модуль отдаёт только данные и события. Геометрия экрана
  (10 строк, шаг 8, x 14) описана в §1 и вынесена в `kVisibleRows`, но не
  применяется.
* **Нет печатных названий**: `name` — ствол; `LVLNAMES.SLU` не читается и не
  встроен, `SET_LEVEL_NAME` доступен через `campaign_level()`.
* **Нет картинок/превью уровней** и нет `BMP.IDX`-ассетов (`plate`,
  `menuarrw`).
* **Порядок и разблокировка — модель** (§2): кампанийный порядок вместо скана
  каталога и пороговые правила вместо отсутствия блокировки.
* **Секреты — модель в двух местах**: `secrets_found` осмысленно только для
  играемой миссии (tally сбрасывается на старте уровня), `secrets_declared` —
  статическая таблица из §8 (в `campaign_state` таких полей нет). Пары
  «секрет найден / объявлен» по уровням игра не хранит.
* **`completed`** не различает «миссия пройдена» и «миссия идёт»: счётчик этого
  не говорит; при `counter = 25` все 24 миссии формально завершены (правило
  `mission < counter`).
* **Времена** — только чтение через `Records`; запись `GUNMETAL.HI` и экран
  конца уровня — `ui/records.*`.


## Кадр экрана

Рендер 320×200 — `ui/level_select_render.*`
(`docs/LEVEL_SELECT_RENDER.md`); CLI: `--level-select --png OUT`.
