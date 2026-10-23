# Рантайм-кэш сэмплов (M7.4): модель, API, числа

Статус: **модель реализована и проверена**, но это **не побайтовый порт**
оригинала. `engine/src/audio/sample_cache.{h,cpp}`, CTest `sample_cache`
(label `quick`), тест — `engine/tests/test_sample_cache.cpp`.

Коротко: кэш принимает имя из `SOUND_SAMPLE_TABLE` (300 слотов), на промахе
декодирует SOU0/SOU1/SOU2 из `sounds/SOU.IDX` (фолбэк `MUSIC/SOU.IDX`),
держит запись, пока `refcount > 0`, и выгружает записи с `refcount == 0` в
порядке LRU, если вставка выходит за бюджет. Пока **не подключён** к микшеру,
3D-звуку и сейвам.

Что доказано кодом/данными, что гипотеза — §4–§5. Адреса ниже — из
`docs/SOUND_NAMES.md`, `docs/SOUND_FIELDS.md` и `ROADMAP.md` (M7.4); Ghidra
для этой задачи не запускалась, листинг читался через
`tools/re/disrange.py`/`~/.cache/gunmetal-re/gundx.asm`.

---

## 1. Что взято из M7.4 (оригинал)

| что | адрес | как используется |
|---|---|---|
| записи сэмплов, 300 × `0x1c` | `0x83d960` | модель 300 слотов; слот 0 в оригинале не выдаётся |
| имена уровня, 300 × 9 байт | `0x4c9c90` | `gm::SoundSampleTable` (`res/sbt.h`), запись 12 `.sbt` |
| счётчик ссылок из `.sbt` | `0x4cebd8` | `count == 0` трактуется как «сэмпла нет» |
| второй счётчик (дубликаты имён) | `0x4cf1a8` | отказ при повторном `release`, а не крэш |
| бюджет памяти | `0x4c8770` | бухгалтерия `0x100 + size` на запись |
| имя → слот | `0x40b9c0` | хвост после последнего `\`/`:`, ≤ 8 символов, регистронезависимо (`0x416850`) |
| имя → файл | `0x40ade0` | `sounds`, при промахе `music` (`0x40ae3c`) |
| загрузка/выгрузка | `0x40af30` / `0x40b400` | `bytes += 0x100 + size`, `bytes -= 0x100 + size` |
| промах — тишина | `0x40bec0`, `0x40bd80`, `0x40ace0` | `nullopt` + текст, без исключений |

### 1.1. Чего в оригинале нет

В `0x40af30`, `0x40b400` и геттере `0x40a9a0` переменная `0x4c8770` только
складывается/вычитается/читается — **сравнения с лимитом нет** (проверено по
всем трём xref: `0x40a9a0`, `0x40b3ad`/`0x40b3c1`, `0x40b43c`/`0x40b443`).
Поэтому «бюджет» в M7.4 — это счётчик, а не доказанный предел. Наш предел —
модель (§5).

---

## 2. API

```cpp
namespace gm::audio {

class SampleCache {
public:
    static constexpr std::size_t kEntryOverhead = 0x100;
    static constexpr std::size_t kDefaultBudgetBytes = 16u * 1024u * 1024u;

    struct Handle { std::uint32_t slot; std::uint64_t serial; };
    struct Stats {
        std::size_t entries, bytes;
        std::uint64_t hits, misses, evictions;
        std::size_t peak_bytes;
    };
    struct EntryInfo {
        std::uint32_t slot;
        std::string name;
        std::uint32_t refcount;
        std::size_t bytes;
        std::uint64_t last_used;
        const Sound* sound;
    };

    static std::optional<SampleCache>
    open(const std::filesystem::path& game, const SoundSampleTable& table,
         std::string& error,
         std::size_t bytes_limit = kDefaultBudgetBytes);

    std::optional<Handle> acquire(std::string_view name,
                                  std::string* error = nullptr);
    bool release(const Handle& handle, std::string* error = nullptr);
    std::optional<EntryInfo> find(std::string_view name) const; // без загрузки
    const Sound* sound(const Handle& handle) const;
    Stats stats() const;
    std::size_t bytes_limit() const;
};

}
```

`Handle` — `slot + serial`; `serial` меняется при каждой новой загрузке слота,
поэтому старый хэндл не может отпустить/прочитать перезагруженную запись.
`Sound` — канонический 16-битный PCM из `audio/mixer.h` (готов к
`Mixer::play(sound.ref())`).

---

## 3. Семантика

1. **Резолв имени.** Хвост после последнего `\`/`:`, обрезка до 8 символов,
   ASCII-регистронезависимое сравнение со слотами таблицы. Порядок — слоты
   `0..299`, побеждает первый. Не найдено → `nullopt` +
   `not in SOUND_SAMPLE_TABLE`.
2. **`count == 0`.** Слот считается отсутствующим (как предзагрузка
   `0x40b6c2`, которая берёт только `counts[i] > 0` и непустое имя).
3. **Попадание.** Запись уже в кэше → `refcount++`, `last_used` обновляется,
   декода нет (`hits++`).
4. **Промах.** `catalog.find` (`sounds`, затем `music`) → чтение BIGFILE →
   декод:
   * `SOU1` — `audio::decode` + `make_sound` (исходная частота, дубликаты
     32 кГц сняты, `clamp(value >> 5)`);
   * `SOU2` — `decode_sou2` + `make_sound`;
   * `SOU0` — `parse_sou1_header` + `make_sound_u8`.
   Затем вставка с `refcount = 1`.
5. **Бюджет.** Стоимость записи `0x100 + 2 * sizeof(int16_t) * samples`
   (PCM уже канонический). Если `bytes + need > limit`, выгружаются записи с
   `refcount == 0` от самого старого `last_used` к новому. Если все записи
   заняты — `nullopt`, текст начинается с `budget:`.
6. **`release`.** Уменьшает `refcount`. При `refcount == 0` запись остаётся в
   кэше (её можно выгрузить), но повторный `release` — отказ
   `release: ... double release` (аналог `0x4cf1a8`, не крэш). Протухший
   `serial` или уже выгруженный слот — тоже отказ с текстом.
7. **`find`.** Только чтение: не грузит, не меняет `refcount`, не двигает LRU
   и не увеличивает `hits`/`misses`.
8. **Детерминизм.** Никакого времени, случайности и порядка хэш-таблицы:
   `std::map` по слоту + `std::list` LRU, счётчики монотонны. Одинаковая
   последовательность операций даёт одинаковый `Stats`.

Ошибки не бросают исключений: все неудачи — `nullopt`/`false` + строка.

---

## 4. Числа теста (команды и вывод)

Сборка (каноническая, `docs/BUILD.md`):

```sh
cmake --build build/sandbox -j 8
ctest --test-dir build/sandbox -j 4
```

Полный прогон: **120/120 passed**, `Total Test time (real) = 313.72 s`
(`full` 630.96 s·proc, `quick` 175.99 s·proc), новый тест входит в `quick`.

```sh
$ ctest --test-dir build/sandbox -R '^sample_cache$' -V
```

Фактический вывод теста (установка `/mnt/d/games/Gunmetal 3dfx`):

```
sample_cache: level0 table 300 slots, 137 resolvable names in sounds/SOU.IDX
sample_cache: picked slot 0 updown, slot 1 select, slot 2 vleft
sample_cache: costs updown=12800 B/6272 fr, select=12800 B/6272 fr, vleft=9472 B/4608 fr
sample_cache: refcount entries=1 bytes=12800 hits=1 misses=1 evictions=0 peak=12800 limit=16777216
sample_cache: evict entries=1 bytes=12800 hits=0 misses=2 evictions=1 peak=12800 limit=12800
sample_cache: budget entries=2 bytes=25600 hits=0 misses=3 evictions=0 peak=25600 limit=25600
sample_cache: missing entries=0 bytes=0 hits=0 misses=1 peak=0 limit=16777216
sample_cache: determinism entries=2 bytes=25600 hits=1 misses=2 evictions=0 peak=25600 limit=16777216
sample_cache: ok
```

Что именно проверено:

* имена не зашиты: тест сканирует `level0.sbt`, берёт слоты с `count > 0`,
  чьи записи реально есть в `sounds/SOU.IDX` (137 из 137 именованных слотов),
  и использует первые три — `updown`, `select`, `vleft`;
* `acquire` дважды → `hits=1, misses=1, entries=1`, `refcount=2`, один декод;
  два `release` → `refcount=0`, запись остаётся (выгружаема), третий
  `release` → `double release`;
* бюджет `12800` (ровно размер `select`): `updown` с `refcount=0` выгружен при
  вставке `select` (`evictions=1`, `find(updown)` пуст);
* бюджет `25600` (`updown + select`, обе заняты): `vleft` (9472 B) →
  `nullopt` с `budget:`, `entries/bytes/evictions` не изменились;
* `nosuchsample` → `nullopt` + `SOUND_SAMPLE_TABLE`; копия таблицы с
  `counts[0] = 0` → `nullopt` + `count 0`; `find("nosuchsample")` не меняет
  `misses`;
* две одинаковые последовательности на двух кэшах → одинаковые `Stats`.

Стоимость: `updown`/`select` — 6272 кадра (8 кГц), 12800 B = `0x100` + 6272×2;
`vleft` — 4608 кадров, 9472 B.

Python-оракул: `python3 regress.py --mirror --json` →
`{"passed": 115, "total": 115, "seconds": 48.7, "full": false, ... "failed": []}`.

---

## 5. Гипотезы (явно)

| # | гипотеза | почему |
|---|---|---|
| 1 | **бюджет по умолчанию 16 МиБ** | в оригинале `0x4c8770` — только счётчик, лимита нет (§1.1). 16 МиБ — наша константа: половина `SOU.DAT` (26 738 998 B), хватает уровню и заметна для выгрузки |
| 2 | **LRU по `last_used`** | `0x40b400` вызывается точечно; обхода «самый старый» в коде не прослежено. Порядок — модель для детерминизма |
| 3 | **стоимость `0x100 + 2·samples`** | `0x100` взят из `0x40af30`; `FUN_0040dc00(ptr)` не разобран, а наш PCM — канонический S16, тогда как оригинал держит 32-битные аккумуляторы SOU1 и формат 4 SOU2. Абсолютные байты с оригиналом **не сравнимы** |
| 4 | **сериализация хэндла** | `slot + serial` — наша защита от use-after-release; в оригинале хэндл — просто номер записи `0x83d960` |
| 5 | **`find` без загрузки** | в оригинале `0x40b940` отдаёт имя по хэндлу; «найти в кэше по имени, не грузя» — наш API |
| 6 | **дубликаты имён** | побеждает первый слот таблицы; `0x4cf1a8` в оригинале переиспользует запись. В поставке дубликатов внутри уровня нет (5800 слотов, 528 имён) |
| 7 | **`SOU0` поддержан** | в `sounds/SOU.IDX` 13 записей SOU0; задача называла SOU1/SOU2, SOU0 добавлен тем же путём `make_sound_u8`, отдельных goldens нет |

---

## 6. Ограничения

* Не подключён к `Mixer`, `audio/engine_3d.*`, платформенному выводу и
  сейвам; это следующий шаг, а не часть M7.4-модели.
* Нет выгрузки по времени/простаиванию — только по бюджету и только
  `refcount == 0`.
* Бюджет фиксируется в `open`; сеттера нет (тесты создают отдельный кэш).
* Нет потокобезопасности: один аудиопоток, как в оригинале.
* Нет динамических имён (`0x40b9c0` умеет занимать свободный слот под имя,
  которого нет в таблице); кэш разрешает только имена таблицы уровня.
* Нет `WARN`-лога на промах: вызывающий получает текст ошибки (§3), политика
  «один раз на `(level, slot, name)`» остаётся за ним.
* Декод всегда полный, без `Sou1Stream`: для больших записей память
  выделяется целиком (как и в модели кэша).
* Байты — модельные (§5 п.3), не побайтовое совпадение с `0x4c8770`.

---

## 7. Файлы и тест

| файл | роль |
|---|---|
| `engine/src/audio/sample_cache.h` | API, `Handle`, `Stats`, `EntryInfo` |
| `engine/src/audio/sample_cache.cpp` | резолв, декод SOU0/1/2, refcount, LRU, бюджет |
| `engine/tests/test_sample_cache.cpp` | реальные `level0`/`dm1`-имена из данных, refcount, выгрузка, бюджет, промахи, детерминизм |
| `engine/CMakeLists.txt` | `src/audio/sample_cache.cpp` в `gm_engine` |
| `engine/tests/CMakeLists.txt` | CTest `sample_cache`, label `quick` |

Тест использует `level0.sbt` (137 именованных слотов, все разрешаются в
`sounds/SOU.IDX`); `dm1`-таблица проверяется тем же кодом в `sample_table`,
отдельный кэш для неё не нужен.
