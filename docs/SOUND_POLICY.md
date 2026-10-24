# Политика отсутствующих звуков (M7.6, срез 2)

Статус: **проверено тестом** (`sound_policy`, CTest label `quick`,
~1.0 с). Слой — над `res/sound_catalog.*` (резолв, срез 1) и
`audio/sample_cache.*` (декод + refcount, M7.4), сам рантайм не меняет.
Доказательная база поведения оригинала — `docs/SOUND_NAMES.md` (адреса VA,
данные `SOU.IDX`, «тишина, не падение»); здесь — модель C++ и её числа.

## 0. Что именно моделируется

`SoundPolicy` отвечает на три вопроса, которых в C++ не было как отдельной
проверяемой единицы:

1. куда разрешилось имя из `SOUND_SAMPLE_TABLE` — `sounds` (прямое попадание),
   `music` (фолбэк) или никуда (`missing`);
2. можно ли это имя загрузить (через `SampleCache`, без правок кэша);
3. что делает `play`-подобный вызов при отсутствии звука: **тишина — успех**,
   а не исключение, не падение и не спам в stderr.

API (`engine/src/audio/sound_policy.h`):

```cpp
enum class SoundLookup { sounds, music_fallback, missing };

struct SoundPolicyStats {
    int requested, resolved, fallback, missing, silent_frames;
};

class SoundPolicy {
    void set_table(const SoundSampleTable&);   // таблица уровня (0x4c9c90)
    void set_budget(std::size_t bytes);        // бюджет кэша (модель)
    bool set_root(const std::filesystem::path& game, std::string* error = nullptr);
    bool ready() const;
    SoundLookup lookup(std::string_view name, int* entry_id = nullptr) const;
    std::optional<SampleCache::Handle> load(std::string_view name,
                                            std::string* error = nullptr,
                                            int* entry_id = nullptr);
    bool play(std::string_view name, std::string* error = nullptr);
    void note_missing(std::string_view name);
    SoundPolicyStats stats() const;
    std::vector<std::string> missing_names() const;
    std::size_t warned_count() const;
};
```

## 1. Доказано кодом и данными (не модель)

Из `docs/SOUND_NAMES.md` (перепроверено там же на листинге и данных):

* цепочка резолва `0x40ade0` → `0x40af30` → `0x40d1a0` → `0x480180`:
  имя приводится к верхнему регистру ASCII, ключ — первые **8 байт**
  (`0x40d1e6`, `0x40d1f0`), сравнение — двумя dword (`0x40d6b3`);
* единственный фолбэк — другой каталог: `sounds` → `music` (`0x40ae10`,
  `0x40ae3c`), в поставке он не срабатывает ни для одного имени уровня;
* коды промаха: `-1` — нет записи в каталоге (`0x40af0c`); `-2` — нет
  свободной записи (`0x40af61`) или не читается заголовок контейнера
  (`0x40afa6`, `0x40b3ec`); всё заканчивается выходом без падения;
* лог у имён из `.sbt` **отсутствует**: `0x40b6e9` просто сохраняет код;
  `Warning: Unable to load sample …` есть только у 7 жёстко зашитых UI-имён
  (`0x48f770`…`0x48f822`) и у сэмплов песни (`0x40ac55`);
* данные: 999 записей `sounds/SOU.IDX`, 3092 — `MUSIC/SOU.IDX`; 5800
  именованных слотов на 36 уровнях, 528 разных имён после регистро-свёртки,
  **0** ненайденных (`python3 tools/re/sound_names.py --report`).

Проверка в C++ (тест `sound_catalog`, `M7.6` срез 1) уже пинует 5800/528/0;
этот срез пинует числа **по уровням** (см. §3).

## 2. Модель: что добавлено и чего в оригинале нет

| элемент | решение | почему |
|---|---|---|
| `entry_id > 0` | 1-базовая позиция записи в `SOU.IDX` | номер записи `0x83d960` выдаётся по порядку загрузки и из индекса не наблюдаем; контракт «индекс слота ≠ номер записи» (`docs/SOUND_NAMES.md` §3.4) сохранён |
| `-1` | нет записи в `sounds` и `music`, либо политика не готова | `0x40af0c` |
| `-2` | запись есть, но играть нельзя: размер < 16 байт (заголовок SOU), бюджет кэша, декод, имя не слот таблицы уровня | `0x40afa6`/`0x40af61`; «нет свободной записи» в модели = нет места в бюджете `SampleCache` |
| `missing_names()` | 8-байтовые ключи игры (`basename` после `\`/`:`, верхний регистр, обрезка до 8), уникальные, по возрастанию | тот же ключ, что у `0x40d1f0`; `nosuchsample` и `NOSUCHSAMPLE` — одно имя |
| `Warn` | один раз на ключ **в пределах объекта** | оригинал для имён таблицы молчит (§5 `SOUND_NAMES.md`); лог — наше улучшение, тишина остаётся паритетной |
| `lookup` vs `note_missing` | `lookup` только считает счётчики; запись в список/лог делают `load`/`play`/`note_missing` | проход по 300 слотам не должен засорять диагностику |
| `silent_frames` | счётчик `play`, завершившихся тишиной | «тишина — успех»: `play` возвращает `true` и при промахе |
| `resolved+fallback+missing == requested` | инвариант после каждого вызова | имя может разрешиться в индексе, но не загрузиться — это `missing` (звука нет) |
| бюджет/LRU | из `SampleCache` (его собственная модель, `docs/SAMPLE_CACHE.md`) | кэш не правится |

`play` после успешной загрузки сразу отдаёт ссылку (`release`): микшера здесь
нет, моделируется одноразовый вызов. `set_root` при ошибке оставляет объект
**не готовым** (никакого полусостояния); повторный `set_root` сбрасывает
каталог, кэш и карты позиций.

## 3. Числа теста

Команда (в worktree):

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_sound_policy_tests -j 4
ctest --test-dir build/sandbox -R '^sound_policy$' --output-on-failure
```

Вывод теста (реальные уровни, `/mnt/d/games/Gunmetal 3dfx`):

```
sound_policy: sounds index 999 entries, music fallback: yes
sound_policy: level0.sbt slots=300 named=137 empty=163 sounds=137 music=0 missing=0
sound_policy: dm1.sbt slots=300 named=135 empty=165 sounds=135 music=0 missing=0
sound_policy: updown entry=582 frames=6272 rate=16000 resolved=2
sound_policy: missing nosuchsample silent=2 list=1 warned=1
sound_policy: budget=0 updown -> load code=-2, play=silence
sound_policy: synthetic broken entry -> code=-2
sound_policy: ok
```

| уровень | слотов | с именем (`count>0`, имя ≠ "") | пустых | `sounds` | `music` | `missing` |
|---|---:|---:|---:|---:|---:|---:|
| `level0.sbt` | 300 | **137** | 163 | 137 | 0 | 0 |
| `dm1.sbt` | 300 | **135** | 165 | 135 | 0 | 0 |

Числа сняты тем же правилом, что и загрузчик `0x40b670` (слот считается при
`count > 0` и непустом имени), оракулом:

```sh
python3 - <<'EOF'
import sys; sys.path.insert(0,'tools/re'); sys.path.insert(0,'.')
import sound_names as sn
g='/mnt/d/games/Gunmetal 3dfx'
idx=sn.load_index(g+'/sounds/SOU.IDX', g+'/sounds/SOU.DAT')
midx=sn.load_index(g+'/MUSIC/SOU.IDX', g+'/MUSIC/SOU.DAT')
for lv in ['level0','dm1']:
    names=sn.level_names(g+'/'+lv+'.sbt')
    print(lv, len(names), {a: sum(1 for _,n,_ in names
          if sn.resolve(idx,n,None,midx)['archive']==a)
          for a in ('sounds','music')})
EOF
# level0 137 {'sounds': 137, 'music': 0}
# dm1    135 {'sounds': 135, 'music': 0}
```

Совпадение с `docs/SOUND_NAMES.md` §0: глобально «через sounds: 5800; через
music: 0; ненайденных: 0». **137 — это число именованных слотов `level0`, а не
«95»**: 95 — имена, у которых есть левая колонка в мёртвом `sounds.sub`
(§0/§4 `SOUND_NAMES.md`), а 137 = 95 + 42 (вторая колонка). Совпадение 137/95
чисто числовое, связи между ними нет.

Что проверяет тест (`engine/tests/test_sound_policy.cpp`):

1. проход по **всем 300 слотам** `level0` и `dm1`: числа выше, `requested ==
   named`, `silent_frames == 0`, `missing_names()` пуст (свип не логирует);
2. существующее имя `updown`: `lookup == sounds`, `entry_id > 0`,
   `SampleCache` отдаёт **6272 кадра** (16 кГц), `resolved` растёт на
   `lookup + load`;
3. `dm1a01` — только в `MUSIC/SOU.IDX`: `lookup == music_fallback`,
   `entry_id > 0`, `fallback` растёт; загрузка даёт `-2` (это не слот таблицы
   `level0`) без падения;
4. отсутствующее имя: `lookup == missing`, `-1`, загрузка — `nullopt`, не
   падает; `play` дважды (включая другое написание того же 8-байтового ключа)
   → `silent_frames == 2`, `missing_names().size() == 1`,
   `warned_count() == 1` (лог один раз на имя);
5. `-2` по бюджету: `set_budget(0)` + `updown` → `load` даёт `-2`,
   `error` начинается с `budget:`, `play` — тишина;
6. `-2` по битому заголовку: синтетическая установка в
   `temp_directory_path()` с одной записью индекса длиной 4 байта →
   `lookup == missing` с `-2`, `load` — `nullopt`, `play` — тишина
   (запись есть, но контейнера в ней нет);
7. негатив: несуществующий каталог → `set_root` = false, `ready()` = false,
   `lookup/load/play` дают промах/тишину без крэша; валидная установка без
   `set_table` → `set_root` = false с текстом про `SOUND_SAMPLE_TABLE`;
   повторный `set_root` на плохой путь сбрасывает готовность;
8. детерминизм: одинаковая последовательность на двух объектах даёт равные
   счётчики и одинаковый список; список уникален и отсортирован.

## 4. Ограничения (сознательно вне среза)

* **Не подключено к рантайму**: ни `main.cpp`, ни `Mixer`, ни 3D-очередь
  (`0x40bec0`/`0x40bd80`) не вызывают `SoundPolicy`; это проверяемая единица
  политики, а не путь воспроизведения.
* **`music`-фолбэк проверяется по индексу, а не воспроизведением**: тест
  доказывает, что имя из `MUSIC/SOU.IDX` разрешается и получает номер записи;
  декод музыки идёт обычным путём `SampleCache` только если имя есть в
  таблице уровня (у `dm1a01` её нет — ожидаемый `-2`).
* **Нет приоритетов, громкости, пана, 3D-затухания** — `play` не аргументы
  микшера, а «одноразовый вызов»; `silent_frames` — счётчик нашего слоя.
* **`entry_id` — позиция в индексе, не номер записи `0x83d960`**: оригинал
  выдаёт номера по порядку загрузки (`0x40af3c`), поэтому формула «индекс+1»
  к нему не применяется; наружу слот по-прежнему отдаётся индексом 0..299.
* **`-2` «нет свободной записи»** моделируется бюджетом/нечитаемым
  заголовком; 300-входовый предел `0x83d960` (`0x40af55`) в модели не
  воспроизводится, потому что кэш `SampleCache` живёт по бюджету.
* **Алиасы `sounds.sub` не применяются** — в v1.36 загрузчик мёртв
  (`0x40b510` ждёт счётчик, `docs/SOUND_NAMES.md` §4); если появится другой
  билд, это отдельная работа.
* **Лог `Warn` — добавление движка**, а не паритет: оригинал для имён из
  `.sbt` молчит. Уровень лога настраивается общим `GM_LOG`; при `off`
  `missing_names()`/`warned_count()` всё равно работают.

## 5. Воспроизведение и файлы

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_sound_policy_tests -j 4
ctest --test-dir build/sandbox -R '^sound_policy$'
python3 regress.py --mirror --json      # 115/115, ~77 с
```

Файлы среза: `engine/src/audio/sound_policy.{h,cpp}`,
`engine/tests/test_sound_policy.cpp`, `docs/SOUND_POLICY.md`; правки
`engine/CMakeLists.txt` (один `.cpp` в `gm_engine`) и
`engine/tests/CMakeLists.txt` (цель `gm_sound_policy_tests`, тест
`sound_policy`, label `quick`). `sample_cache.*`, `sou1.*`, `main.cpp`,
`config.*` не менялись.
