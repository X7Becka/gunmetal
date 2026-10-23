# `audio/music`: громкость и переходы музыкального трека

Статус: **модуль и тест готовы**; в `main.cpp` не подключён (проводку CLI
делает основной агент). Модуль: `engine/src/audio/music.{h,cpp}`, тест
`engine/tests/test_music.cpp` (цель `gm_music_tests`, CTest `music`, метка
`quick`). Чистый DSP: без SDL, PulseAudio, потоков и блокировок.

Задача — `M7.5`. Сейчас `--music FILE` декодирует `.sog` через
`MUSIC/SOU.IDX`/`SOURCE` и отдаёт все клипы в `AudioDevice` подряд с
постоянной громкостью (`engine/src/main.cpp`, `play_music`); громкость входа
декодера — константа 32. Переходов между треками нет, проверить их было
нечем. Этот модуль выносит громкость/фейды/кроссфейд в тестируемое место
поверх готового `PcmClip` (`res/sou2.h`) и не меняет `mixer.*`.

## 1. Что доказано про громкость

**Код и данные:**

* Громкость входа декодера `SOU2` — глобал `0x84a68c`, диапазон **0…32**
  (`res/sou2.h`, `decode_sou2(..., volume = 32)`; `docs/FORMATS.md` §5.2.1).
  Это **не** кривая затухания, а множитель таблицы шагов/усиления внутри
  кодека; эталонные FNV-хэши корпуса сняты при 32 (максимум).
* Музыка идёт через голос-слот 8 из девяти (`0x84e068 + 8*0x50`,
  `0x84e31c`), остальные восемь — эффекты (`docs/SOUND_FIELDS.md` §3).
  Для голоса доказаны громкости каналов L/R **1…32** (`+0x2c`/`+0x30`) и
  формула панорамы (`0x40a4b0`/`0x40a690`); музыкальный слот использует тот
  же цикл микшера.
* Музыкальный путь играет **всегда 32000 Гц** и не читает `sample_rate` из
  заголовка `SOU2`: DirectSound-буфер создаётся с 32000
  (`0x405d8c`/`0x405e4b`), `FUN_00409330` отдаёт 1024 кадра без шага. Поэтому
  `L01BONUS` (66304 кадра, в заголовке 16000) звучит 2.072 с, а не 4.144 с
  (`docs/SOU2.md` §2).
* Однотрековый `.sog` зацикливается: после конца микшер снова выбирает
  индекс 0 (`DAT_0084a688 = 1`, `DAT_0084a684 = 0`) — `docs/SOU2.md` §3.
* `+0x34..+0x3c` слота 8 — состояние SOU2-синтеза («музыка A/B/C»), роль
  **не разобрана**, в `docs/SOUND_FIELDS.md` помечена гипотезой.

**Модель (не оригинал):** кривая фейдов, длительности, law кроссфейда и
расписание треков по уровням в оригинале не прослежены. Всё, что ниже
описывает закон `gain(k)`, — наша модель, выбранная так, чтобы её можно было
проверить числами и чтобы она не клиповала.

## 2. API

```cpp
namespace gm::audio {

class MusicTrack {
public:
    static constexpr int kMinRate = 1;          // часы модуля
    static constexpr int kMaxRate = 192000;
    static constexpr int kMinSourceRate = 1000; // как у mixer.cpp
    static constexpr int kMaxSourceRate = 384000;

    bool load(const PcmClip& clip, int sample_rate, std::string* error = nullptr);
    bool load_at(const PcmClip&, int source_rate, int sample_rate,
                 std::string* error = nullptr);

    bool play();          // с кадра 0, огибающая 1
    void stop();
    bool playing() const;
    bool finished() const; // клип кончился ИЛИ гейт фейда закрыт

    void   set_gain(double gain); // 0..1, NaN/inf отвергается
    double gain() const;          // master * огибающая текущего трека
    double master_gain() const;
    void   fade_in(double seconds);   // 0 -> 1, линейно по амплитуде
    void   fade_out(double seconds);  // текущая -> 0

    void advance(int frames);         // часы без рендера
    int  mix_into(std::span<float> out, int channels); // additive, 1|2 канала

    void crossfade_to(const PcmClip& next, double seconds);
    bool crossfade_to_at(const PcmClip&, int source_rate, double seconds);
    struct CrossfadeInfo { bool active; double progress, old_gain, new_gain; };
    CrossfadeInfo crossfade() const;

    int  sample_rate() const;
    std::uint64_t frames() const;
    double duration_seconds() const;
    double position_frames() const;
    double position_seconds() const;
    const std::string& last_error() const;
};
}
```

Семантика, которую легко перепутать:

* `sample_rate` в `load` — **выходная** частота (часы `advance`/`mix_into`);
  источник — `clip.sample_rate`. Для `L01BONUS`/`LOADSCRN` нужен
  `load_at(clip, 32000, rate, ...)`: заголовок у них 16000, но игра играет
  32000 (`docs/SOU2.md` §2).
* `advance()` и `mix_into()` двигают **одни и те же** часы; на один
  интервал вызывается что-то одно.
* `mix_into` не перезаписывает буфер, а прибавляет к нему и не клипует:
  модуль гарантирует только, что вклад источника из `[-1,1]` при
  `gain <= 1` остаётся в `[-1,1]` (линейный кроссфейд — выпуклая
  комбинация). Итоговый clamp — за вызывающим.
* `play()` сбрасывает огибающую в 1. `fade_in()` до `play()` запоминается и
  применяется при старте (`load -> fade_in -> play` — рабочий порядок).
* `finished()` — «было играем и слышать больше нечего»: конец клипа или
  закрытый гейт фейда. Явный `stop()` — не `finished`; `play()` после
  `finished()` перематывает в 0 и снова играет.
* Ошибки `void`-методов (`set_gain`, `fade_*`) — в `last_error()`; состояние
  при отказе не меняется.

## 3. Закон фейдов и почему кроссфейд не клипует

Все рампы линейны по **амплитуде**, шаг — один выходной кадр,
`N = max(1, round(seconds * sample_rate))`:

```
fade_in(N):   gain(k) = k / N              k = 1..N
fade_out(N):  gain(k) = 1 - k / N
crossfade(N): old(k) = 1 - k / N, new(k) = k / N
```

Кроссфейд — линейная смесь: `out = g * ((1-t)*a + t*b)`, `t = k/N`.
Для `|a|,|b| <= 1` и `g <= 1` это выпуклая комбинация, поэтому

```
|out| <= g * max(|a|, |b|) <= 1
old^2 + new^2 = (1-t)^2 + t^2 <= 1
```

— пик не превышает полной шкалы, сумма квадратов весов ≤ 1. Equal-power
(`sin/cos`) дал бы `1.414` на коррелированных треках и клип; цена линейного
закона — провал ≈ −3 дБ в середине. Это осознанный выбор модели.

## 4. Числа теста

Сборка и прогон:

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox -j 8
./build/sandbox/engine/tests/gm_music_tests
```

Фактический вывод (`music: all checks passed`, rc=0):

```
fade: clock 91 Hz (the game tick), clip 10 s @ 1000 Hz
fade_in(1.0) @91: steps=91 first=0.010989010989 mid(45)=0.494505494505 end=1.000000000000 |mid-0.5|=0.005495
fade_out(1.0) @91: end=0.000000000000 playing=0 finished=1
fade_in(1.0) @100: gain(50/100)=0.5
fade_in(0.5) before play @100: gain(25/50)=0.500000000000
crossfade: ramp=10000 frames, mid old=0.500000000000 new=0.500000000000 progress=0.500000000000
crossfade: max(old^2+new^2)=1.000000000000 (linear law, <= 1)
crossfade: peak=0.997740746 (must be <= 1)
crossfade window [4950,5050): bin440=24.999/24.999 bin660=12.500/12.500 rms=0.395278/0.395278 predicted=0.395285
crossfade end: active=0 old=0.000000000000 new=1.000000000000 playing=1
finish: clip=0.100 s (100 frames), gate=50 frames, mix_into=50 frames, playing=0 finished=1 gain=0.000000000000
rewind: playing=1 finished=0 gain=1.000000000000 position=0.000 s
natural end: playing=0 finished=1 position=0.100 s
negative mix_into without load: written=0 error="music: mix_into() without a loaded track"
negative load empty clip: "music: clip has no frames"
negative load source rate 0: "music: bad source rate 0"
negative load 3 channels: "music: channels must be 1 or 2"
negative load output rate 0: "music: output rate 0 is outside 1..192000"
negative set_gain(NaN): master=1.000000000000 error="music: set_gain(NaN/inf) ignored"
negative fade_in(-1): gain=1.000000000000 error="music: fade_in() wants a finite non-negative duration"
negative crossfade without playback: active=0 error="music: crossfade_to() needs an active track"
real L01BONUS: archive=24524 bytes, header=16000 Hz, 132608 samples (66304.000000 frames/ch), 4.144000 s at header rate
real L01BONUS: load_at(32000, 32000) frames=66304 duration=2.072000 s (header duration would be 4.144000 s)
real L01BONUS: at 32000 it is 2.072000 s, at the header rate 4.144000 s
real L01BONUS: mixed=4096 frames peak=0.999969 rms(L)=0.171327 playing=1
real .sog: Dm1A.sog has 8 tracks, first=Dm1A01
real .sog track Dm1A01: header=32000 Hz, frames=91136, duration=2.848 s
music: all checks passed
```

Что именно проверяют числа:

* **91 шаг** — такт игры (91 Гц) как часы: `gain(k) = k/91` с точностью
  `1e-9`, монотонно, на 45-м шаге `0.4945` (≈ 0.5, отклонение `1/182` от
  целочисленной середины). Чётные 100 шагов дают ровно `0.5` (`1e-12`).
  `fade_out` — зеркало и доходит до 0; после него `finished()=1`.
* **Кроссфейд 440/660 Гц** при 1000 Гц: на середине `old=new=0.5`;
  `max(old^2+new^2)=1.0` (закон), пик `0.9977 <= 1`; окно 100 кадров даёт
  DFT-бины `24.999` и `12.500` (амплитуды 1.0 и 0.5), т.е. **оба трека
  слышны**, и RMS `0.395278` сходится с формулой `0.395285` (< 0.01%).
* **Реальный `L01BONUS`**: 24524 байта, 66304 кадра, при 32000 — 2.072 с
  (в 2 раза короче заголовочных 4.144 с), после 4096 кадров пик 0.999969,
  RMS 0.171327 — данные не тишина и не клип.
* **Реальный `.sog`**: `Dm1A.sog` — 8 треков, первый `Dm1A01` — 91136
  кадров, 2.848 с.
* **Негативы**: NaN/inf громкость, отрицательные `fade_in`/`fade_out`/
  `advance`, `mix_into` без загрузки, нулевые частоты, 3 канала, пустой клип
  и кроссфейд без воспроизведения — возвращают 0/`false`, пишут текст в
  `last_error()` и не падают.

## 5. Ограничения

* **Нет точной кривой оригинала.** Доказаны шкала 0…32 и места громкости
  голоса/декодера; форма фейдов, их длительности и law кроссфейда в
  `gundx.exe` не прослежены. Модель — линейная по амплитуде, не equal-power.
* **Нет расписания треков по уровням.** `.sog` даёт список, но когда и с
  каким переходом игра начинала следующий трек, не разобрано (кроме
  однотрекового цикла, `docs/SOU2.md` §3).
* **Нет цикла и сэмпл-точного шва.** Модуль играет клип один раз; зацикливание
  одиночного `.sog`, историю/lookahead фильтра `0x19d`/`0x184` и синтез
  `0x8534c0` он не воспроизводит. `PcmClip` — уже выход суб-полосного
  декодера, а не слышимый PCM (`docs/SOU2.md` §1); этот тест проверяет
  громкость/переходы, а не качество звука.
* **Нет вывода в PulseAudio.** `mix_into` пишет `float`, устройство
  (`platform/audio.cpp`) принимает `int16`; преобразование и реальный
  `--music --fade-in` — проводка CLI.
* **Ресемплинг линейный**, без фильтра; частоты вне 1…192000 и источники вне
  1000…384000 отвергаются.
* **Не потокобезопасно**, как и `Mixer`: один экземпляр на аудиопоток.

## 6. Что осталось для CLI

Основной агент подключает модуль в `main.cpp` (файл сабагенту запрещён):

* `--music NAME --fade-in SEC` — `load/load_at -> fade_in -> play`;
  `--fade-out SEC` перед выходом; `--crossfade SEC` между треками `.sog`;
* `load_at(clip, 32000, device_rate)` для музыки, а не заголовочный
  `sample_rate` (иначе `L01BONUS`/`LOADSCRN` играют вдвое дольше);
* `mix_into` + приведение `float -> int16` (с clamp) в pull-колбэк
  `AudioDevice` (`docs/AUDIO_OUTPUT.md`), чтобы музыка микшировалась с
  эффектами, а не очередью `queue()`;
* по желанию — `crossfade()`/`gain()` в `--dump`/лог и решение о цикле
  одиночного `.sog` (сейчас модуль принципиально одноразовый).
