# `audio/mixer`: детерминированный software-микшер

Статус: **готово**, отдельный модуль, в `main.cpp` не подключён.
Модуль: `engine/src/audio/mixer.{h,cpp}`, тест
`engine/tests/test_mixer.cpp` (цель `gm_mixer_tests`, CTest `mixer`,
метка `quick`, ~0.12 с).

Микшер не использует SDL и ничего не знает о платформе. На вход подаются
уже декодированные сэмплы SOU0/SOU1/SOU2, на выходе — фиксированный
interleaved 16-bit stereo с частотой `init(rate)` (по умолчанию 22050 Гц).

## 1. API

```cpp
namespace gm::audio {

enum class SampleFormat : std::uint8_t { U8, S16, S32 };

struct SoundRef {                 // не владеет памятью
    const void* data;             // interleaved
    std::uint32_t frames;         // кадров на канал
    std::uint32_t sample_rate;    // исходная, не выходная
    std::uint8_t channels;        // 1 или 2
    SampleFormat format;
    std::int32_t shift = 5;       // S32: acc >> shift -> int16
};

struct Sound {                    // владеющая каноническая форма
    std::vector<std::int16_t> samples;
    std::uint32_t sample_rate;
    std::uint8_t channels;
    std::uint32_t frames() const;
    SoundRef ref() const;
};

struct Vec3 { float x, y, z; };
struct Listener { Vec3 position; float yaw; };

struct PlayParams {
    bool loop = false;
    float gain = 1.0f;            // 0..4, NaN/inf -> тишина
    float pan = 0.0f;             // -1 L, 0 C, +1 R
    std::optional<Vec3> position; // задан -> 3D, пуст -> 2D
    std::optional<Vec3> velocity; // хранится, Doppler не применяется
};

class Mixer {
public:
    // Capacity, not a copy of the original: `0x84e068` holds 9 slots of 0x50
    // (8 effect voices + 1 music stream, `docs/SOUND_FIELDS.md` 2).  C++ keeps
    // 32 so a level can layer more one-shots; behaviour of the shipped data is
    // unaffected.
    static constexpr int kMaxVoices = 32;
    static constexpr float kMaxGain = 4.0f;
    static constexpr float kRefDistance = 1.0f;
    static constexpr float kRolloff = 1.0f;
    static constexpr float kMaxDistance = 1000.0f;

    struct Stats { /* frames, silent, started, finished, stopped,
                      dropped, clipped, peak, active, max_active */ };

    bool init(std::uint32_t sample_rate = 22050); // 1000..192000
    int  play(const SoundRef&, const PlayParams& = {});
    int  play(const Sound&,   const PlayParams& = {});
    bool stop(int voice);
    void stop_all();
    void set_listener(const Listener&);
    void mix(std::int16_t* out, std::size_t frames);
    int  voices() const;
    const Stats& stats() const;
};
}
```

* `play` возвращает **id голоса** (> 0) либо `-1`: плохой звук или пул занят.
  Id не переиспользуются, поэтому устаревший `stop(id)` не остановит новый
  голос. Слоты внутри фиксированы (0..31), голоса всегда суммируются в
  порядке слотов.
* `mix(out, n)` не выделяет память, не бросает исключений, не вызывает
  `new`/`vector` и не зависит от истории запусков. `out == nullptr` и
  `n == 0` — no-op.
* `init` при недопустимой частоте возвращает `false` и **не трогает**
  текущее состояние.
* Буфер `SoundRef::data` должен жить, пока играет голос; безопасная форма —
  владеющий `Sound` и `play(sound, …)`.

## 2. SOU0 / SOU1 / SOU2 -> один формат

Все три кодека приводятся к `Sound` (interleaved `int16_t` + исходная
частота + 1/2 канала). Никакого второго декодера здесь нет: SOU1
по-прежнему декодируется `audio/sou1.cpp`/`res/sou1.cpp`.

| вход | адаптер | преобразование |
|---|---|---|
| SOU0, сырой unsigned 8-bit PCM | `make_sound_u8(pcm, rate, ch, err)` | `(v - 128) * 256` |
| SOU1, 32-bit аккумуляторы, 32 кГц слоты | `make_sound(const Sou1Sound&, err, shift = 5)` | `clamp(acc >> shift)`, дубликаты слотов отбрасываются |
| SOU2, готовый `PcmClip` | `make_sound(const PcmClip&, err)` | копия `int16` |

SOU1 хранит слоты на 32 кГц с повторами `32000 / rate` (1/2/4 для
32/16/8 кГц). Адаптер берёт первый слот каждой группы и получает ровно
`Sou1Sound::sample_rate` кадров/с; поэтому линейный ресемплер работает с
настоящей исходной частотой, а не со ступенчатым 32 кГц потоком. Частоты,
не делящие 32000 нацело, отклоняются: в поставке SOU1 есть только 8/16/32
кГц.

`SampleFormat::S32` + `shift` оставлены в `SoundRef` для случая, когда
вызывающий хочет играть аккумуляторы без промежуточной копии. `shift = 5`
совпадает с `fnv1a64_pcm16` манифеста `docs/manifests/sou1-fnv.json` и
`res::sou1_to_wav`.

## 3. Ресемплинг и микширование

Позиция в источнике — 32.32 fixed point:

```
step = (source_rate << 32) / output_rate        // на play()
pos += step                                     // на каждый выходной кадр
i    = pos >> 32                                // кадр источника
frac = (pos >> 16) & 0xffff                     // 16-битная дробь

s0 = sample(i); s1 = sample(i + 1)
s  = s0 + ((s1 - s0) * frac >> 16)
```

* На границе цикла `s1` берётся из кадра 0 (`loop`).
* В конце нецикличного звука `s1 = s0` (hold последнего значения, а не
  затухание в ноль), чтобы последний кадр был точным.
* `sample()` понимает U8/S16/S32; для S32 применяется `clamp(acc >> shift)`.
* Финальный аккумулятор округляется перед сдвигом, как `0x8610d0`
  (`(acc + 0x10) >> 5`): в Q15-раскладке это `(acc + (1 << 14)) >> 15`.
  При единичном гейне (`32768 = 1.0`) результат совпадает с прежним
  усечением, поэтому голдены не меняются. Точная раскладка голосов и
  сэмплов — `docs/SOUND_FIELDS.md`.
* `step` для равных частот равен ровно `2^32`, поэтому синтетические
  фикстуры воспроизводятся без дробных ошибок.

Гейны считаются в Q15 (`32768 = 1.0`), суммирование — в `int64`:

```
acc_l += s_l * gain_l;   acc_r += s_r * gain_r
out_l  = clamp(acc_l >> 15, -32768, 32767)
```

32 голоса по 32767 с гейном 4 дают максимум ~2^47, переполнения `int64`
нет; клиппинг считается в `Stats::clipped` по каждому каналу.

## 4. Панорама и 3D

2D-панорама — линейный закон (гипотеза, см. §6):

```
p  = clamp(pan, -1, 1)
gl = g * (1 - max(0, p))
gr = g * (1 + min(0, p))
```

Центр даёт `gl = gr = g`, край — `g` в одном канале и 0 в другом.

3D включается только заданным `PlayParams::position`. Слушатель:
`position`, `yaw` вокруг +Y;

```
forward = ( sin(yaw), 0,  cos(yaw))
right   = ( cos(yaw), 0, -sin(yaw))
r       = source.position - listener.position
d       = |r|
```

Затухание по расстоянию (inverse clamped, `ref = 1`, `rolloff = 1`,
`max = 1000`):

```
d_eff = min(d, 1000)
att   = 1                                        , d_eff <= 1
att   = 1 / (1 + 1 * (d_eff - 1))                , d_eff > 1
```

Направление влияет только на панораму в горизонтальной плоскости:

```
p3d = (r.x * right.x + r.z * right.z) / d        , d > 1e-4
p   = clamp(pan + p3d, -1, 1)
g   = clamp(gain, 0, 4) * att
```

Источник строго справа (`r` вдоль `right`) даёт `p3d = +1`, слева `-1`,
спереди/сзади — 0. Высота на панораму не влияет, но входит в `d`.

Защита: `gain`, `pan`, координаты и `yaw`, не являющиеся конечными
числами, заменяются на 0 (гейн — на тишину); всё зажимается в допустимый
диапазон. NaN/inf не попадают в выход.

`velocity` сохраняется в голосе, но **не** применяется: Doppler в
оригинале не трассирован.

## 5. Golden и тест

`engine/tests/test_mixer.cpp` (метка `quick`, ~0.12 с) проверяет:

* адаптеры SOU0/SOU1/SOU2 на синтетике (все три частоты SOU1, дубликаты,
  чётность кадров, негативы);
* импульс, DC, сумму 1/2 голосов, точные значения при равных частотах;
* loop (рампа 100/200/300/400 зацикливается кадр в кадр), `stop` по id,
  естественное завершение;
* pan L/C/R, distance 1/10, направление, поворот слушателя, высоту;
* ресемплинг 11025 -> 22050 (0.5 кадра за шаг) и 44100 -> 22050;
* лимит 32 голоса, 33-й отклоняется, `voices_dropped`, клиппинг 32x2000;
* побитовый детерминизм: два независимых микшера и повторный `init` на
  одном и том же сценарии дают одинаковый буфер;
* негативы: `init(0/999/192001)`, null `out`, 0 кадров, null/data 0/rate 0/
  channels 0|3/неизвестный format, `gain = inf`, `gain = NaN`,
  `pan = NaN`;
* реальные SOU1 записи из `sounds/SOU.IDX`.

Golden — FNV-1a 64 по первым **4096 стерео-кадрам** (8192 little-endian
`int16`), выход 22050 Гц, `loop = false`, `gain = 1`, `pan = 0`, слушатель
в нуле:

| запись | источник | кадров источника | FNV-1a 64 |
|---|---|---:|---|
| `ADON` | 8000 Гц, моно | 3004 | `08ac82e8766481a9` |
| `INTRO` | 32000 Гц, стерео | 576000 | `afd8dc5a7461c163` |

`ADON` целиком укладывается в 8280 выходных кадров, поэтому его golden
включает и хвост тишины после конца клипа. Хеши сняты этим же тестом;
это **регрессионные** golden микшера, а не паритет с игрой. Сырые сэмплы
SOU1 при этом уже имеют отдельный доказанный паритет 983/983
(`docs/SOU1_CPP.md`), так что расхождение хеша означает изменение
адаптера/ресемплера/гейнов, а не кодека.

Тест пропускает golden-часть, если игра не установлена; синтетика и
негативы выполняются всегда.

## 6. Доказано / гипотеза

**Доказано (в этом дереве):**

* SOU1-сэмплы: `audio/sou1` + манифест `docs/manifests/sou1-fnv.json`,
  983/983, побитово (`docs/SOU1_CPP.md`).
* SOU0 — сырой unsigned 8-bit PCM, SOU2 — interleaved 16-bit PCM
  (`docs/FORMATS.md` §5, `res/sou2.h`).
* Арифметика микшера: целочисленный 32.32-ресемплер, Q15-гейны, `int64`
  сумма, клип в `int16`; два прогона побитово равны (тест).
* Фиксированный пул 32 голоса, порядок суммирования не зависит от истории,
  `mix` без аллокаций.
* Линейная интерполяция и правило цикла (последний -> первый кадр)
  однозначно зафиксированы тестом.

**Гипотеза (константы не трассированы по exe):**

* Выходная частота 22050 Гц — выбрана API по умолчанию; оригинальный
  микшер игры работает на 32 кГц (`docs/FORMATS.md` §5.2, `0x409dde`).
* Гейн `acc >> 5` — наблюдаемая полная шкала аккумулятора (~2^20), а не
  доказанный гейн игры.
* Линейный закон панорамы, `ref = 1`, `rolloff = 1`, `max = 1000` —
  простая модель; формула `0x40a4b0`/`0x40a690` не дизассемблирована.
  Эти адреса в коде **не используются**.
* Панорама только по горизонтали; вертикаль не влияет.
* 32 голоса — требование API; в оригинале структура голоса 0x50 байт и
  8 голосов (`docs/SOU1.md` §4).
* `velocity` не влияет на звук (нет Doppler).

## 7. Сборка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_mixer_tests -j 32
ctest --test-dir build/sandbox -R mixer --output-on-failure
python3 regress.py
```

`mixer` — `quick`, 0.12 с; `python3 regress.py` — 113/113, ~78 с.

## 8. Что осталось

* Интеграция в `main.cpp`/`platform/audio.cpp` (SDL callback, загрузка
  SOU из BIGFILE, привязка к игровым событиям) — вне области этой задачи;
  `main.cpp` не менялся.
* Точные константы оригинала: гейн микшера, панорама и 3D-затухание
  (`0x40a4b0`, `0x40a690`) — нужен дизассемблер/эмулятор, как для SOU1.
* Нет Doppler, фильтров, реверба и интерполяции выше линейной.
* SOU1-адаптер материализует весь клип в `int16` при `play`; потоковый
  zero-copy голос поверх 32 кГц слотов — возможное продолжение.
* Golden для реальных SOU0/SOU2 в этом тесте не снимались (их декодеры
  покрыты `sou`/`sou2` тестами); при интеграции стоит добавить.
