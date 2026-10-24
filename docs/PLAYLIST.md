# `audio/playlist`: списки песен `MUSIC/*.sog`

Статус: **модуль и тест готовы**; к `Music`/микшеру и в `main.cpp` не
подключены (проводку делает основной агент). Модуль:
`engine/src/audio/playlist.{h,cpp}`, тест `engine/tests/test_playlist.cpp`
(цель `gm_playlist_tests`, CTest `playlist`, метка `quick`). Чистые данные:
без декодирования, микшера, SDL и платформенного аудио.

Задача — `M7.5` срез 2. `.sog` — текстовый плейлист (имя сэмпла на строку),
а сэмплы лежат не в `sounds/SOU.DAT`, а во **втором BIGFILE-архиве**
`MUSIC/SOU.IDX`/`SOU.DAT` (3092 записи, все `SOU2`, ~75 МБ,
`docs/FORMATS.md` §9). Модуль закрывает разрыв «файл → индекс записи →
смещение/размер»: `main.cpp --music` до сих пор обходит `.sog` сам и ищет
имена в архиве по месту, проверить это было нечем.

## 1. Что доказано (данные и код)

**Формат (данные, `docs/FORMATS.md` §9, `gm/sog.py`):**

* Все **651** файл разбираются как текст «одно имя на строку»; `gm/sog.py`
  делает `parse -> to_bytes` побайтово точным для **651/651** (включая
  верхнерегистровые `.SOG`), то есть модель строк (CRLF, пустые строки,
  внешние пробелы) подтверждена round-trip'ом, а не «на глаз».
* В `MUSIC/` **653** записи: 651 плейлист + `SOU.IDX` + `SOU.DAT`. Из
  плейлистов **598** `.sog` и **53** `.SOG` — регистр расширения значим для
  обхода каталога: `glob("*.sog")` теряет 53 файла (эта ошибка уже была,
  `tools/qa/doc_numbers.py`).
* Ссылки имён ведут во второй архив: `MUSIC/SOU.IDX` — **3092** записи,
  имена **уникальны** (3092/3092 после фолда).
* Имена в архиве — поле `char name[8]` (`docs/FORMATS.md` §1): в корпусе
  длина 6 (3071), 7 (1), 8 (20). Ни одного имени длиннее 8.
* **Дубликаты — данные, не ошибка.** В 651 плейлисте **998** лишних
  вхождений (имя встречается `k` раз → `k-1`), в **131** плейлисте.
  Пример: `L26I.sog` — 112 строк, блоки по 4 имени повторяются
  (`L26J01..04` ×2, затем `L26K..`), 72 повтора; `Dm2A.SOG` — 32 строки,
  28 повторов. Дедупликация изменила бы песню.
* **Пропуски — вырезанный контент.** **123** вхождения **95** разных имён в
  **25** песнях не резолвятся (ни в `MUSIC/SOU.IDX`, ни в `sounds/SOU.IDX`);
  это ровно те 25 песен, о которых §9 говорит «движок выдал бы
  `Unable to load sample…`». Независимая сверка оракулом:

  ```sh
  python3 gm.py sog ~/.cache/gunmetal-re/game/MUSIC \
      --music ~/.cache/gunmetal-re/game/MUSIC/SOU.IDX | tail -1
  # sog: 651 songs, 25 with unresolved samples
  ```

* **Регистр.** **1054** вхождения (**816** разных имён в **188** плейлистах)
  записаны в смешанном регистре (`Dm1B01`, `winner`), а в индексе они в
  верхнем (`DM1B01`, `WINNER`). Точное (регистрозависимое) сравнение нашло бы
  **4038** из **5211** вхождений (77.5 %), фолд — **5088**.
  `winner.sog` (единственная строка `winner`) резолвится в запись **3091**
  `WINNER` размером 1 341 632 байта. Код оригинала складывает ключ в верхний
  регистр (`0x40ae0c`, `docs/SOUND_NAMES.md` §3.1), поэтому фолд — не наша
  вольность.
* Пустых плейлистов в корпусе **0** из 651, строк с комментариями нет
  (иначе 95 «пропавших» имён были бы больше) — отказ на пустом файле (§2)
  это модель, а не наблюдение.
* Во всех 651 файлах CRLF; 1227 пустых строк и 321 строка с внешними
  пробелами — обе модели срезаются, круглый round-trip от этого не страдает.

**Код (C++):** `load_playlist` использует `gm::parse_sog` (`res/text.h`) —
тот же разбор строк, что у CLI `--music`, поэтому C++ и Python-оракул
сходятся по определению, а не по совпадению.

## 2. Модель (наше решение, не восстановленное поведение)

| Вопрос | Решение | Почему |
|---|---|---|
| Сравнение имён | ASCII-фолд в нижний регистр; байты ≥ 0x80 не трогаются | ключ оригинала верхнерегистровый (`0x40ae0c`); корпус — ASCII |
| Дубликаты имён | сохраняются в порядке файла | 998 повторов в корпусе — это музыка, а не мусор |
| Дубль в архиве | побеждает первая запись (наименьший индекс) | в поставочном индексе дублей нет; правило нужно для крафта |
| Имя длиннее 8 байт | грузится, `found=false` всегда | поле BIGFILE `name[8]`, такое имя не может совпасть |
| Строка длиннее 255 байт | файл отвергается с текстом | граница доверия до разбора (как `res/sub.cpp`) |
| Пустой/пробельный файл | отвергается, `out` не меняется | песня без сэмплов — битый файл; так же `gm::parse_sog` |
| Пропавшее имя | не ошибка: `found=false`, `index_in_music=-1` | оригинал печатает `Unable to load sample` и играет дальше |
| Пустой архив | `resolve_playlist` = `false` + текст | «битая установка» ≠ «всё пропало» |
| Обход каталога | `<game>/MUSIC`; если его нет — сам путь | голый каталог песен тоже работает |
| Расширение | сравнивается без регистра (`.sog`/`.SOG`) | 53 файла `.SOG` иначе теряются |
| Индекс | 0-based позиция в `BigFile::entries()` | наблюдаемо из IDX; номер слота 0x83d960 — нет |

Модель JSON-совместима без переименований: запись — это
`{"name":…, "index_in_music":…, "bytes":…, "found":…}`.

## 3. API

```cpp
namespace gm::audio {

inline constexpr std::size_t kMaxNameBytes = 255;     // граница доверия
inline constexpr std::size_t kArchiveNameBytes = 8;   // BIGFILE name[8]

struct PlaylistEntry {
    std::string name;        // как в файле (trim, регистр сохранён)
    int index_in_music = -1; // 0-based в BigFile::entries(), -1 = нет
    int bytes = 0;           // размер записи в MUSIC/SOU.DAT, 0 = нет
    bool found = false;
};

struct Playlist {
    std::vector<PlaylistEntry> entries;   // порядок файла, дубликаты сохранены
    std::filesystem::path path;
    bool resolved = false;

    int found_count() const;
    int missing_count() const;
    int duplicate_count() const;          // лишние вхождения (k-1 на имя)
    std::int64_t found_bytes() const;     // Σ bytes по найденным
    std::vector<std::string> missing_names() const; // уникальные, ВЕРХНИЙ, сорт
};

bool load_playlist(const std::filesystem::path& file, Playlist& out,
                   std::string* error);                    // .sog -> записи
bool resolve_playlist(Playlist&, const BigFile& music,
                      std::string* error = nullptr);       // имя -> запись
std::vector<std::filesystem::path> playlist_files(const std::filesystem::path& game);
} // namespace gm::audio
```

Контракты отказов (текст в `*error`, `out` не изменён): `not a .sog file: X`,
`cannot open X`, `empty playlist: X`, `sample name longer than 255 bytes in X`,
`music archive has no entries`. Отсутствующий `MUSIC/SOU.IDX` отвергает
`BigFile::open` (`cannot open index: X`). `resolve_playlist` идемпотентен.

## 4. Числа теста

```sh
cmake --build build/pl --target gm_playlist_tests -j 4
./build/pl/engine/tests/gm_playlist_tests      # playlist: 852 checks, ok
ctest --test-dir build/pl -R "^playlist$"      # Passed, 7.2 s
python3 regress.py --mirror --json             # 115/115
```

Файлы: **651** (`598` `.sog` + `53` `.SOG`), первые три `Dm1A.sog`,
`Dm1B.sog`, `Dm1C.sog`, последние `s01q.sog`, `s01r.sog`, `winner.sog`.
`playlist_files(MUSIC)` даёт те же 651 имён, что `playlist_files(game)`.

Пять реальных плейлистов (индекс и размер первых записей):

| файл | имён | found | missing | дубликатов | Σ байт | первые записи (index, bytes) |
|---|---:|---:|---:|---:|---:|---|
| `Dm1A.sog` | 8 | 8 | 0 | 0 | 244 204 | `Dm1A01`(0, 31 148), `Dm1A02`(1, 23 716), `Dm1A03`(2, 31 876) |
| `Dm1B.sog` | 8 | 7 | 1 | 0 | 236 876 | `Dm1B01`(8, 33 312), `Dm1B02`(9, 34 272), `Dm1B03`(10, 34 404) |
| `L26I.sog` | 112 | 112 | 0 | 72 | 2 116 524 | `L26J01`(2732, 18 988), `L26J02`(2733, 18 904), `L26J03`(2734, 19 060) |
| `Dm2A.SOG` | 32 | 32 | 0 | 28 | 648 652 | `Dm2A01`(73, 19 520), `Dm2A02`(74, 19 532), `Dm2A03`(75, 20 852) |
| `winner.sog` | 1 | 1 | 0 | 0 | 1 341 632 | `winner`(3091, 1 341 632) |

Весь корпус (651 файл, 3092 записи архива):

```
names=5211 found=5088 missing=123 bytes=124138728
dups=998 dup_playlists=131 missing_playlists=25 unique_missing=95
case_sensitive_found=4038 mixed_case=1054 distinct_names=3017
top missing: L24H12=4, L10H02=3, L24N04=3, DM1C04=2, L10G08=2
```

`distinct_names=3017` — разных имён (после фолда) во всём корпусе;
`mixed_case=1054` — вхождений в смешанном регистре (см. §1).

`bytes` — сумма по вхождениям, поэтому больше объёма архива (75 МБ):
повторённый сэмпл играется снова и считается снова.

## 5. Ограничения

* **Ничего не проигрывает и не декодирует**: модуль не знает про `PcmClip`,
  `MusicTrack`, `Mixer` и `AudioDevice`; поток — отдельная задача
  (`docs/MUSIC.md`, `docs/SOU2.md`).
* **Не подключён** к `audio/music.*`, микшеру, `main.cpp`, конфигу и
  `--music`; CLI по-прежнему сам обходит `.sog` и ищет имена в архиве по
  месту.
* **Нет очерёдности/повторов/перемешивания**: порядок — порядок строк файла.
  Зацикливание однотрекового `.sog` (`docs/SOU2.md` §3) и расписание музыки
  по уровням не моделируются.
* **Нет проверки содержимого**: `bytes`/`index_in_music` — только IDX;
  действительно ли блоб `SOU2`, решает `decode_sou2` (`res/sou2.h`).
* **Нет кэша карты имён**: `resolve_playlist` строит `map` на каждый вызов.
  Для 651 файла это дёшево (тест целиком 7.2 с, включая чтение всех файлов
  и `BigFile::open`), для покадровой работы не предназначено.
* **Только чтение**: `.sog` не пишется и не правится; побайтовый round-trip
  остаётся за `gm/sog.py` (`--rebuild`).
* Имя не валидируется по charset: любые байты кроме пробельных допустимы,
  не-ASCII сравнивается побайтово (в корпусе таких нет).

## 6. Ссылки

`docs/FORMATS.md` §1 (BIGFILE), §9 (`.sog` и `MUSIC/SOU.DAT`);
`docs/MUSIC.md` (фейды/кроссфейд трека); `docs/SOU2.md` §2/§3;
`docs/SOUND_NAMES.md` §3.1 (ключ в верхнем регистре); `gm/sog.py`;
`ROADMAP.md` `M7.5`.

## 7. CLI `--playlist FILE` (2026-10-24)

```
$ gunmetal --playlist Dm1B.sog
playlist: Dm1B.sog entries=8 found=7 missing=1 bytes=236876
  Dm1B01        8    33312
  Dm1B02        9    34272
  ...
  Dm1C04       -1        0  (missing)
```

Относительный путь ищется в `<game>/MUSIC`; архив — `<game>/MUSIC/SOU.IDX`.
Печатает `found/missing/bytes` и каждую строку (`index_in_music`, байты,
пометка `(missing)`). Ничего не проигрывает. CTest `cli_playlist`,
`cli_playlist_missing`.
