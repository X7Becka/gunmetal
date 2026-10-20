# `SOU1` в C++: слой `audio/sou1`

Статус: **готово**, 983/983 записей, прямой и обратный проход.
Модуль: `engine/src/audio/sou1.{h,cpp}`, тест
`engine/tests/test_sou1_audio.cpp` (цели `gm_sou1_audio_tests`,
CTest `sou1_audio` / `sou1_audio_full`).

## 1. Зачем отдельный слой

Кодек уже портирован в `engine/src/res/sou1.cpp` (`decode_sou1_raw`,
`decode_sou1`, `sou1_to_wav`) и проверен на 983/983. Таблицы и ниббл-цикл
живут там в одном экземпляре; второй экземпляр — это ровно тот способ,
которым кодеки расходятся.

`audio/sou1` добавляет то, чего у `res` нет:

* `Sou1Sound` с **исходной** частотой (8000/16000/32000), а не 32 кГц слотов;
* потоковое чтение `Sou1Stream` кусками без копирования всего буфера;
* явные ошибки контейнера (truncated / odd в strict / overrun) без исключений;
* строгий разбор заголовка отдельно от декода (`parse`).

Реализация вызывает `gm::decode_sou1_raw` из `res/sou1.h`; ни таблицы
`0x850500` / `0x850540..0x8522c0`, ни BIGFILE здесь не дублируются.

## 2. API

```cpp
namespace gm::audio {

struct Sou1Sound {
    std::uint32_t sample_rate;       // 8000 / 16000 / 32000 (источник)
    int channels;                    // 1 или 2
    std::vector<std::int32_t> samples; // interleaved 32-бит слоты, дубликаты сохранены
};

struct Sou1Format {
    std::uint32_t sample_rate, flags, declared_length;
    std::size_t payload_bytes;       // чётное, min(length, available) & ~1
    int channels, slots_per_byte;    // 8 / 4 / 2
};

struct Sou1Options {
    int volume = 31;   // строка кривой 0..31, 31 = полная громкость
    bool strict = false; // запретить нечётный declared_length
};

std::optional<Sou1Format> parse(std::span<const std::uint8_t>, std::string& error,
                                bool strict = false);
std::optional<Sou1Sound> decode(std::span<const std::uint8_t>, std::string& error,
                                Sou1Options = {});
class Sou1Stream; // open(blob, error, options), read(span), rewind(), done()
}
```

`decode` — один вызов на весь payload: состояние `{acc, level}` стартует с
нуля (старт голоса `0x40a5e0`) и не сбрасывается, как и в микшере.

`Sou1Stream::open` разбирает и декодирует запись один раз, затем `read`
копирует только запрошенный кусок в буфер вызывающего. `read` возвращает
число записанных слотов, `0` — конец (см. `done()`).

## 3. Контейнер и формула

16-байтный заголовок: `char magic[4]`, `u32 rate`, `u32 flags`, `u32 length`
(`docs/FORMATS.md` §5). Payload начинается с +16.

```
для каждого байта (старший ниббл первым):
    code  = nibble
    delta = CURVE[base + (level >> 2) + code]   // знаковый dword
    acc   = (acc + delta) mod 2^32
    level = (level + STEP[code]) mod 2^32
    вывести acc `copies` раз
```

* `STEP` (`0x850500`): `-64×4, 128, 256, 384, 512` дважды.
* `CURVE` — одна общая кривая `0x850540..0x8522c0` (118 строк × 16 dword);
  `level` — байтовое смещение в ней, зажима нет.
* `base` — `VOLUME_BASE[volume]` в dword'ах (`0x850480`, 0..31; 31 → 464).
* `copies = slots_per_byte / 2`: 4 / 2 / 1 для 8 / 16 / 32 кГц.
* `slots_per_byte`: 8 при `rate < 10000`, 4 при `rate < 18000`, иначе 2.
* Стерео: байты payload перемешаны по нибблам, каждая пара `A B` даёт
  `left = (A & 0xf0) | (B >> 4)`, `right = ((A << 4) & 0xf0) | (B & 0x0f)`;
  каналы декодируются независимо, затем чередуются `L R L R ...`.
* Длина канала — `payload / channels` байт; выход — `slots_per_byte` слотов
  на байт (дубликаты сохраняются), т.е. 32 кГц.

`Sou1Sound::sample_rate` — исходная частота; чтобы получить кадры источника,
берите каждый `32 / sample_rate`-й слот (1 / 2 / 4).

## 4. Ошибки

| вход | результат |
|---|---|
| нет 4 байт / нет 16 байт | `truncated ...`, `nullopt` |
| magic не `SOU1` (`SOU0`, `SOU2`, мусор) | `not a SOU1 blob`, `nullopt` |
| `rate == 0` | ошибка |
| `flags & ~1` | ошибка |
| `declared_length` > доступно (кроме стерео +1) | `truncated payload` |
| нечётный `declared_length` | в обычном режиме игнорируется хвостовой байт (`& ~1`), в `strict` — ошибка |
| `level` вышел из кривой | overrun, `nullopt` |
| `volume` вне 0..31, `shift` вне 0..31 | ошибка |

Исключений наружу нет: все пути возвращают `optional` + текст в `error`.

## 5. Golden и N/N

FNV-1a 64 по little-endian слотам; `raw` — 32-битные аккумуляторы,
`pcm5` — `clamp(v >> 5)` (как `fnv1a64_pcm16` манифеста). Источник —
`docs/manifests/sou1-fnv.json`, снятый с реальных байт `gundx.exe`
(`0x852b40/0x852c00/0x852cc0`), и независимо пересчитанный `gm/sou1.py`.

| проход | записей | слотов/канал | сэмплов | max abs | raw FNV | pcm>>5 FNV |
|---|---|---|---|---|---|---|
| вперёд | 983 | 67 939 382 | 75 430 904 | 1 048 576 | `538367759d7c7cac` | `156174534588594c` |
| назад | 983 | 67 939 382 | 75 430 904 | 1 048 576 | `bc04189439738fa8` | `c5ef5d8e0eef0d4c` |

Обратный проход идёт по записям в обратном порядке; сэмплы внутри записи —
вперёд. Совпадение счётчиков и пика, но другие агрегаты, доказывают, что
состояние не протекает между записями.

Точечные записи (все частоты, моно/стерео, 400 Б … 1.6 МБ):

| запись | rate | ch | payload | слотов/канал | raw FNV |
|---|---|---|---|---|---|
| SILENCE | 8000 | 1 | 400 | 3200 | `beb46562da647365` |
| ADON | 8000 | 1 | 1502 | 12016 | `9d67e203de04e745` |
| PRAM | 16000 | 1 | 248864 | 995456 | `4a176ea1ca9a0069` |
| SOYBOY | 32000 | 1 | 263500 | 527000 | `3ed95b2a91404631` |
| INTRO | 32000 | 2 | 576000 | 576000 | `0ca1b6274f408593` |
| CUT-EMP | 32000 | 2 | 1629212 | 1629212 | `689ce6f75d8162fe` |
| VRCHAMBR | 32000 | 2 | 188180 | 188180 | `5fa952cce363cc2a` |

Синтетика (8 кГц, volume 31): тишина `4b 0c 20`, максимум `77×4`,
минимум `ff×4`, границы шагов `40 50 60 70 04 08 0c`, стерео `54 76`.
Негативы: пусто, короткий заголовок, чужой magic, rate 0, flags 2, volume
32, обрезанный payload, нечётная длина (strict), overrun.

## 6. Сборка и тесты

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_sou1_audio_tests -j 32
ctest --test-dir build/sandbox -R sou1 --output-on-failure
python3 regress.py
```

* `sou1_audio` — label `quick`, ~0.9 с: синтетика, негативы, семь записей.
* `sou1_audio_full` — label `full`, ~17 с: 983 записи вперёд и назад.
* `sou1` / `sou1_full` — существующий тест `res/sou1` (не изменялся).

## 7. Расхождения и решения

* **Тест назван `test_sou1_audio.cpp`.** Файл `engine/tests/test_sou1.cpp` и
  цель `gm_sou1_tests` уже есть в `master` (коммит `97f656d`, 1483 строки
  golden'ов). Перезапись уничтожила бы проверенное покрытие, поэтому новый
  слой получил отдельный файл и цели; старый тест не тронут.
* **`sample_rate` — исходная.** `res::RawClip` отдаёт 32 кГц (слоты); здесь
  заголовочная частота, а слоты остаются 32 кГц. Это совместимо с Python
  (`decode_entry` возвращает `rate` + слоты).
* **Нечётная длина не ошибка по умолчанию.** 473 записи корпуса объявляют
  нечётный `length`; загрузчик маскирует бит (`0x40b0dd`), и паритет требует
  того же. `strict` включает ошибку для недоверенного ввода.
* **Стерео `length` на 1 больше payload.** У CUT-EMP/CUT-PAM и др. заголовок
  объявляет `payload + 1`; допускается ровно один лишний байт для стерео,
  иначе — `truncated`.
* **Ошибки vs Python.** Python на обрезанном payload молча укорачивает;
  C++ сообщает `truncated`. На корпусе это не срабатывает (0 записей).

## 8. Что осталось

* Формула громкости/панорамы (`0x40a4b0`, `0x40a690`) не переносилась:
  API принимает готовый индекс 0..31.
* Точный гейн микшера при переводе 32-битного аккумулятора в 16 бит не
  трассирован; `pcm5` — наблюдаемая шкала, а не доказанный гейн игры.
* `audio/sou1` не подключён к `main.cpp`/playback: это отдельная задача
  интеграции (файл `main.cpp` вне области этой работы). WAV-писатель
  остаётся в `res::sou1_to_wav`.
* Автоматической сверки с `gm/sou1.py` в runtime нет (по требованию —
  только агрегаты FNV); Python-прогон: `python3 tools/parity_sou1.py`.
