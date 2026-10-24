# Экран лучших времён (`GUNMETAL.HI`) — `M8.4`, срез 2

**Статус:** модуль `engine/src/ui/records.{h,cpp}` + тест `records`
(`engine/tests/test_records.cpp`, CTest label `quick`, **155 проверок**).
Чистая модель поверх таблицы рекордов: строки файла, `duplicate`, минимум по
имени, решение «улучшение», дописывание записи и текст времени. **Без**
отрисовки, шрифтов, состояния экрана, `GameRuntime` и CLI; запись — только по
явно переданному пути.

Канон поведения — `docs/CAMPAIGN_FLOW.md` §6 и `docs/GAME_OVER.md` §5; здесь —
реализация и её числа. Кодек `GUNMETAL.HI` в `game/campaign_state.*`
(`load_hi`/`save_hi`, `docs/CAMPAIGN_STATE.md`) не изменялся: этот модуль —
сторона экрана, константы раскладки сверены `static_assert`.

> Имя файла: `docs/RECORDS.md` уже занят записями `SBT`
> (`SECTOR_STUFF`/`ITEM`/`LIFT`, 436 строк), поэтому срез экрана рекордов
> лежит отдельно. `docs/RECORDS.md` не тронут.

Проверка:

```sh
cmake --build build/sandbox --target gm_records_tests -j 4
./build/sandbox/engine/tests/gm_records_tests
ctest --test-dir build/sandbox -R '^records$' --output-on-failure
python3 regress.py --mirror --json
```

## 1. Что доказано (код игры и данные поставки)

| факт | значение | источник |
|---|---|---|
| файл | `<game>/GUNMETAL.HI`, 576 Б = 9 × `0x40` | §6; данные: 9/9 записей |
| запись | `i32 seconds; char name[60]`, имя — `SET_LEVEL_NAME` (`TESTING RANGE`, `KELLER CORE`), не ствол | §6: `0x45b73c`/`0x45b753` |
| открытие | `0x493630`, строка `0x4be60c`; `"rb"` `0x4a6388`, `"ab"` `0x4b8690` | §6 |
| чтение | `fread(buf, 0x40, 1, f)` | `0x40dc70` |
| сравнение имён | регистронезависимое | `0x416850` |
| минимум по имени | хранится минимум: `TESTING RANGE` 18, а не 146 | `0x45c8d6` |
| улучшение | строго меньше → дописывание записи (`0x40`), старое не удаляется | `0x45c904`..`0x45c94c` |
| результат для экрана | возврат `0x45c880` → `0x83d2c0`, печатается как `YOUR BEST:` | `0x45c08e`, `0x483ce2`, строка `0x4bcb84` |
| подписи экрана | `TIME:` `0x4bcb9c`, `BEST TIME:` `0x4bcb90` (par `0x80d88c`), `YOUR BEST:` `0x4bcb84` | `GAME_OVER.md` §5, макет `0x483a70` |
| формат времени | `0x483f00`: `"%ld : %ld%ld"` — минуты, десятки и единицы секунд; при нуле `UNKNOWN` | строки `0x4bcbdc` и `0x4bcbd4` (в `.exe` лежат подряд: `UNKNOWN\0%ld : %ld%ld`) |
| данные поставки | 146 `TESTING RANGE`, 1570 `KELLER CORE`, 18 `TESTING RANGE`, 109 `POWER STATION`, 18/7 `NATAKA TOWER`, 11 `OIL RIG`, 40 `OIL REFINERY`, 6 `CADTLELTL` | файл, 9/9; FNV-1a64 `0x80bf2f264bebe680` |

`TIME:`/`BEST TIME:`/`YOUR BEST:` — три **разные** величины (§6): время
прохождения (`0x83d638` = тики / 91), par уровня (`0x80d88c`,
`SET_LEVEL_BEST_TIME`) и рекорд игрока (файл). Модуль работает только с
третьей.

## 2. Что модель, а не порт

1. **Списка всех записей в оригинале нет.** `YOUR BEST:` — одно значение для
   уровня, который завершают; экран ищет имя в файле. `best_by_name()` (имя →
   минимум + число записей) и порядок «секунды по возрастанию, затем имя без
   регистра» — наши. Экран показывал бы одну строку на имя уровня.
2. **`duplicate` — производное поле**: имя уже встречалось в более ранней
   записи (сравнение ASCII без регистра). Игра флаг не хранит и не смотрит.
3. **Отрицательное время — отказ** (при чтении, в `better()` и при записи):
   часы игры — `тики / 91` беззнакового счётчика, отрицательного не бывает;
   такое значение в файле считается порчей.
4. **Имя из 60 байт без NUL при чтении усекается до 60 символов.** Оригинал
   читал бы C-строку дальше поля (для последней записи — за конец файла);
   остановка на границе поля — наша.
5. **Больше 9 записей при чтении допустимо** (игра дописывает без предела,
   `"ab"`); предохранитель — 1024 записи / 64 КиБ, чтобы опечатка в пути не
   тянула произвольный файл. Поставка — 9.
6. **Новое имя ≤ 59 байт**, чтобы 60-байтовое поле сохранило терминатор.
   `campaign_state::set_hi_record` разрешает 60 байт — там другая сторона
   (кодек), и это расхождение осознанное.
7. **`append_and_save()` пишет только по переданному пути** и не выводит его
   из `<game>`. `"ab"` создаёт отсутствующий файл (1 запись); файл, размер
   которого не кратен `0x40`, не дописывается — отказ, чтобы не испортить
   таблицу. `better()` при этом не вызывается: решение за вызывающим, как в
   оригинале (`0x45c8d6` сравнивает, `0x45c904` дописывает).
8. **`format_time()` для отрицательного значения — `UNKNOWN`** (ветка нуля);
   печатать нечего, отрицательных времён у игры нет.

## 3. API (`ui/records.h`)

```cpp
struct RecordRow { int seconds; std::string name; bool duplicate; };
struct RecordView { std::string name; int seconds; int entries; };

class Records {
  public:
    static constexpr int kRecordBytes = 0x40, kNameBytes = 60,
                         kShippedRecords = 9, kShippedBytes = 576,
                         kMaxNameBytes = 59, kMaxRecords = 1024;
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    const std::vector<RecordRow>& rows() const;      // в порядке файла
    bool loaded() const;
    std::vector<RecordView> best_by_name() const;    // минимум по имени, сорт.
    int best_time(std::string_view name) const;      // -1 если нет (0x416850)
    bool better(int seconds, const std::string& name, std::string* error = nullptr) const;
    bool append_and_save(const std::filesystem::path& file, int seconds,
                         const std::string& name, std::string* error = nullptr);
    std::string format_time(int seconds) const;      // 0x483f00
    const std::string& last_error() const;
    void clear_error();
};
bool record_name_equal(std::string_view a, std::string_view b) noexcept;
```

Контракт отказов: текст в `error` (если передан) и в `last_error()`; отказ
`load()` **не меняет** ранее загруженные строки, отказ `append_and_save()` не
меняет файл.

## 4. Числа теста (`records`, 155 проверок)

Реальный `GUNMETAL.HI` установки (только чтение; запись — на копию в
`/var/tmp`, `GM_RECORDS_TMPDIR` переопределяет):

```
records: file /mnt/d/games/Gunmetal 3dfx/GUNMETAL.HI bytes=576 rows=9 fnv=0x80bf2f264bebe680
records: rows times=146,1570,18,109,18,7,11,40,6 names=TESTING RANGE|KELLER CORE|TESTING RANGE|POWER STATION|NATAKA TOWER|NATAKA TOWER|OIL RIG|OIL REFINERY|CADTLELTL
records: duplicates=2 rows=2,5
records: best names=7 CADTLELTL=6 NATAKA TOWER=7 OIL RIG=11 TESTING RANGE=18 OIL REFINERY=40 POWER STATION=109 KELLER CORE=1570
records: better 17<18=1 18<18=0 19<18=0 1569<1570=1 18@lowercase=0 17@MixedCase=1 new_name=1 6<7=1
records: append tail bytes 05 00 00 00 54 45 53 54 49 4e 47 20 52 41 4e 47
records: append bytes=576->640 head_same=1 tail=5,TESTING RANGE reread=10 best_TESTING RANGE=5
records: format time 6=0 : 06 18=0 : 18 47=0 : 47 109=1 : 49 1570=26 : 10 0=UNKNOWN
records: negatives size 575=refused 577=refused
records: deterministic rows=9 best=7 text=0 : 18
records: 155 checks, ok (rows=9 best=7 duplicates=2)
```

Что именно проверено:

* **9 строк**, времена `146,1570,18,109,18,7,11,40,6` и все 9 имён — по
  записям; флаги `duplicate` стоят ровно на строках 2 (`TESTING RANGE`,
  18 после 146) и 5 (`NATAKA TOWER`, 7 после 18), всего 2;
* **`best_by_name`**: 7 различных имён, сумма `entries` = 9, у `TESTING RANGE`
  и `NATAKA TOWER` по 2 записи; порядок — 6, 7, 11, 18, 40, 109, 1570;
  `best_time`: `TESTING RANGE` **18** (не 146), `KELLER CORE` **1570**,
  `NATAKA TOWER` **7**, `testing range` = 18 (регистр), неизвестное имя = -1;
* **`better`**: 17 < 18 → true, 18 при 18 → false (строго меньше), 19 при 18 →
  false, 1569 < 1570 → true, 1570 при 1570 → false, 6 < 7 → true; `18` при
  `testing range` → false, `17` при `Testing Range` → true, неизвестное имя →
  true; после вызовов файл и строки не изменились (решение ничего не пишет);
* **`append_and_save`** на копии: 576 → **640** байт (+`0x40`), первые 576
  байт побайтово те же, новая запись — последняя: `05 00 00 00` + `TESTING
  RANGE` + нули до 60; повторное чтение — **10** строк, `duplicate` у новой
  записи, `best_time("TESTING RANGE")` = **5**, `entries` = 3, текст `0 : 05`;
  отдельно — дописывание в отсутствующий путь создаёт файл в 64 байта
  (`"ab"`), и он читается как 1 запись;
* **`format_time`** (`0x483f00`): 6 → `0 : 06`, 18 → `0 : 18`, 47 → `0 : 47`,
  109 → `1 : 49`, 1570 → `26 : 10`; 0 → `UNKNOWN`, -1 → `UNKNOWN` (модель);
  границы 59/60/61/600/3599/3600 → `0 : 59`/`1 : 00`/`1 : 01`/`10 : 00`/
  `59 : 59`/`60 : 00`;
* **негативы с текстом**: нет файла; **575** и **577** байт
  (`not a multiple of 64`); запись с `seconds = -1`; 60-байтовое имя при
  записи (`at most 59`); пустое имя; имя с NUL; отрицательное время в
  `append_and_save` и в `better`; дописывание в 575-байтовый файл
  (`refusing to append`) — файл после всех отказов побайтово тот же;
* **усечение**: поле имени из 60 байт `A` без NUL читается как 60 символов;
  **0-байтовый файл** — пустая таблица (0 строк, `best_by_name` пуст,
  `better()` = true для любого имени);
* **детерминизм**: два чтения дают те же строки, тот же порядок
  `best_by_name()`, тот же текст (`0 : 18`).

## 5. Ограничения

* **Не подключено** к фронтенду, экрану конца уровня (`0x447748`,
  подрежим 5), `GameRuntime`, меню и CLI. Ни `YOUR BEST:`, ни `BEST TIME:`,
  ни `TIME:` не рисуются — модуль даёт только числа и текст.
* **Нет пикселей и шрифтов**: макет `0x483a70` (одиночный) и `0x483550`
  (табличный `TEAM`/`TOTAL`) не моделируются, фон `levelend`/`levelen2`
  (`0x4bcbc8`/`0x4bcb54`) не читается.
* **`YOUR BEST:` — значение, а не список.** Полная таблица и её сортировка —
  наша надстройка (§2.1); на живом экране её нет.
* **Нет записи в игру по своей инициативе**: `append_and_save()` пишет только
  по переданному пути. Автоматического «конец уровня → дописать рекорд» нет —
  это дело вызывающего (сейчас такого вызывающего нет вовсе).
* **`par` (`BEST TIME:`) и время прохождения (`TIME:`)** не входят в модуль:
  par живёт в `0x80d88c` (модель `SET_LEVEL_BEST_TIME`), время — в `0x83d638`
  (`docs/CAMPAIGN_FLOW.md` §6).
* **Дубликаты не схлопываются** при записи (как в оригинале): файл растёт по
  `0x40` на каждое улучшение; чистки/переупорядочивания нет.
* Сравнение имён — **ASCII** без регистра. Локаль/`_stricmp` оригинала на
  не-ASCII байтах не проверялась; в поставке имена ASCII.
* `UNKNOWN` в `format_time(0)` взят из пары строк `0x4bcbd4`/`0x4bcbdc` в
  `.exe` и описания `GAME_OVER.md` §5; на живой игре нулевой рекорд не
  наблюдался.

## 7. CLI `--best-times` (2026-10-24)

```
$ gunmetal --best-times
records: rows=9
    0     146  TESTING RANGE
    ...
best: names=7
  CADTLELTL 6 0 : 06 entries=1
  NATAKA TOWER 7 0 : 07 entries=2
  ...
  TESTING RANGE 18 0 : 18 entries=2
  KELLER CORE 1570 26 : 10 entries=1
```

Строки печатаются в порядке файла (дубликаты помечены), затем минимум по
имени (без учёта регистра, сортировка по времени) с форматированием
`format_time`. CTest `cli_best_times`, `cli_best_times_best`. Запись
(`append_and_save`) через CLI не вызывается — только чтение.
