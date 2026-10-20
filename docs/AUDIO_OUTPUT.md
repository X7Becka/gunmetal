# `platform/audio`: вывод звука (SDL3 + null)

Статус: **готово** как отдельный platform-модуль; в `main.cpp` не подключён.
Файлы: `engine/src/platform/audio.{h,cpp}` (там же, рядом с существующим
`AudioDevice`), тест `engine/tests/test_audio_output.cpp` (цель
`gm_audio_output_tests`, CTest `audio_output`, метка `quick`, ~0,1 с).
`gm_engine` уже собирает `src/platform/audio.cpp`; `engine/CMakeLists.txt`
менять не пришлось. Игровые модули (`audio/mixer`, `render`, `player`, …) не
изменялись.

`AudioOutput` — это граница producer/consumer для готового interleaved
16-bit PCM: вызывающий кладёт кадры в `push` (или подключает `PullSource`,
например `Mixer::mix`), а backend забирает их через `pull`. SDL3-путь делает
это в аудиопотоке через stream-callback, null-путь вызывает `pull` сам
тест/headless-цикл и поэтому полностью детерминирован.

## 1. API

```cpp
namespace gm {

class AudioOutput {
public:
    enum class Backend : std::uint8_t { Auto, SDL, Null };

    struct Config {
        int sample_rate = 22050;  // 1000..192000
        int channels = 2;         // 1 или 2
        int frames = 1024;        // кадров на запрос устройства / чанк
        Backend backend = Backend::Auto;
        int queue_frames = 0;     // 0 -> 8 * frames (power of two)
    };

    using PullSource = void (*)(void* user, std::int16_t* out, std::size_t frames);

    struct Stats {
        std::uint64_t frames = 0;    // кадров отдано через pull (с тишиной)
        std::uint64_t underruns = 0; // pull не хватило ring+source
        std::uint64_t overruns = 0;  // кадров потеряно в push (ring полон)
        std::string device;          // "null", "SDL: pipewire (…)", "null (SDL: …)"
    };

    static std::optional<AudioOutput> open(const Config&, std::string& error);

    bool start();                 // начать pull; false только если SDL не resume
    void stop();                  // остановить, очередь сохраняется
    void pause(bool paused);      // pull -> тишина, очередь не тратится
    void set_volume(int volume);  // 0..255, clamp; 255 = unity
    int  volume() const;

    bool running() const;
    bool paused() const;

    void set_source(PullSource source, void* user = nullptr);
    std::size_t push(const std::int16_t* samples, std::size_t frames);
    void pull(std::int16_t* out, std::size_t frames);

    std::size_t capacity() const; // ёмкость ring в кадрах
    std::size_t queued() const;

    Backend backend() const;          // фактический, не запрошенный
    const char* backend_name() const; // "sdl" / "null"
    const Config& config() const;
    Stats stats() const;              // снимок, можно звать из любого потока
};

}
```

`open` валидирует всё до обращения к SDL: частота `1000..192000`, каналы
`1/2`, `frames` `1..1<<20`, `queue_frames` `0..1<<22`. Ошибка возвращается
через `error`, объект не создаётся. `Config::queue_frames = 0` даёт
`bit_ceil(8 * frames)`. Null-sink после `open` уже в состоянии `running`
(устройства, которое надо запускать, нет); SDL открывается на паузе и ждёт
`start()`. `start()` на null идемпотентен.

Методы двигают только свои атомарные поля; `stats()` возвращает значение, а
не ссылку, поэтому снимок безопасен при работающем аудиопотоке.

## 2. Backend'ы

| `Backend` | поведение |
|---|---|
| `Null` | SDL не трогается вообще. `pull` вызывает вызывающий; идеально для CTest/headless. |
| `SDL` | Требует SDL3 и устройство. Если SDL3 нет или устройство не открылось — `open` возвращает `nullopt` с причиной. |
| `Auto` | Пытается SDL3; при любой ошибке (нет SDL3, нет драйвера/устройства) отдаёт null-sink и кладёт причину в `Stats::device` (`"null (SDL: …)"`). |

В сборке без SDL3 (`GM_HAVE_SDL3=0`) `Auto` сразу даёт null с
`"null (built without SDL3)"`, а явный `SDL` — ошибку. Падений и
необработанных исключений нет: только `optional` + текст ошибки.

Фактический backend виден в `backend()`/`backend_name()`, а не только в
запросе. `Stats::device` для SDL содержит драйвер и имя устройства, например
`SDL: dummy (System audio playback device)`.

## 3. Ring, callback и семантика

Ring — SPSC на монотонных курсорах `read`/`write` (`std::atomic<uint64_t>`,
acquire/release), ёмкость округляется вверх до степени двойки. Один producer
(`push`), один consumer (`pull`). Сам `pull` не берёт мьютексов, не
аллоцирует и не ждёт устройство; копирование идёт не более чем двумя
`memcpy` (кольцевой разворот). `PullSource` — часть контракта: он тоже
обязан не блокировать.

* `push(samples, n)` кладёт столько кадров, сколько влезает, и возвращает это
  число. Остаток **отбрасывается** и прибавляется к `Stats::overruns`.
  `push(nullptr, n)` и `push(x, 0)` — no-op, счётчики не меняются.
  Пушить можно и до `start`: очередь переживает `stop`.
* `pull(out, n)` забирает `min(n, queued)` кадров. Если ring пуст/короток:
  * есть `PullSource` — он обязан дописать ровно недостающие кадры; underrun
    не считается (источник «успел»);
  * источника нет — остаток заполняется нулями, а `underruns` увеличивается
    **на один** за вызов, а не на каждый недостающий кадр.
  `out == nullptr` и `n == 0` — no-op.
* Вне `start()` и во время `pause(true)` `pull` пишет тишину, **не трогает
  ring** и не считает underrun: это штатная пауза, а не нехватка данных.
  `start()` снимает паузу; `stop()` её сбрасывает.
* `Stats::frames` считает все кадры, реально записанные `pull`, включая
  тишину паузы/underrun'а. Это счётчик отданного наружу, а не «полезного»
  звука.

`PullSource` вызывается consumer'ом. Для null-backend это поток теста, для
SDL — аудиопоток SDL. Поэтому источник не должен блокировать; `Mixer::mix`
подходит (он не аллоцирует и не блокирует). Устанавливать источник нужно до
`start()`: пара `(source, user)` не защищена атомарным обменом.

### SDL3

`open` делает `SDL_InitSubSystem(SDL_INIT_AUDIO)`, затем
`SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, {S16, channels,
freq}, callback, impl)`. Устройство открывается на паузе (так устроен SDL3);
`start` вызывает `SDL_ResumeAudioStreamDevice`, `pause`/`stop` —
`SDL_PauseAudioStreamDevice`.

Callback получает `additional_amount` в байтах, делит на `channels * 2`,
режет запрос на чанки не больше `scratch_frames` (≤ 8192 кадров) и на каждый
чанк делает `pull` в заранее выделенный scratch, затем
`SDL_PutAudioStreamData`. Наш код в callback не аллоцирует и не ждёт
устройство; scratch выделяется в `open`. Внутренним буфером stream при
необходимости управляет сам SDL.

`SDL_DestroyAudioStream` в деструкторе закрывает и stream, и устройство;
перед этим `running = false`, поэтому поздний callback отдаёт только тишину.
`SDL_QuitSubSystem` вызывается ровно для того `Init`, который сделал этот
объект. Если `Init` прошёл, а stream не открылся, subsystem освобождается
перед null-fallback, чтобы неудачная попытка не оставляла аудиоподсистему
включённой.

## 4. Громкость

`set_volume` принимает `0..255` и зажимает значение. `255` — точный
passthrough (никаких округлений), `0` — тишина. Промежуточные значения
считаются целочисленно:

```
out = (int32(sample) * volume) / 255   // truncation toward zero
```

Поэтому, например, `1000 → 501`, `-2000 → -1003`, `32767 → 16447`,
`-32768 → -16448` при `volume = 128`. Это проверено точными значениями, а не
«примерно вдвое». Громкость применяется после ring/source, одинаково к
левому и правому каналу, и не может вызвать клиппинг (255 — максимум).

## 5. Что проверяет тест

`engine/tests/test_audio_output.cpp` (null-backend, все значения точные):

* моно и стерео: побитовое совпадение `push → pull`, границы `int16`;
* `frames = 0`, `pull(nullptr, …)`, `pull(…, 0)`, запрос 100 000 кадров при
  пустом ring — нули без падения, ровно один underrun;
* пауза: тишина, очередь не расходуется, underrun не растёт; после
  `pause(false)` приходят исходные данные;
* громкость `255/128/0` с точными значениями, clamp `-7 → 0`, `999 → 255`;
* overrun: `push(40)` в ring на 32 кадра принимает 32, теряет 8; нехватка
  данных на pull даёт underrun; кольцевой разворот (push после частичного
  чтения);
* повторные `start`/`stop`, данные, накопленные в `stop`, играют после
  следующего `start`;
* `PullSource`: рампа, стык «очередь + источник», пауза не дёргает источник,
  и реальный `audio::Mixer` как источник (DC-звук → 1000 в обоих каналах);
* негативы `open`: rate `0/-1`, channels `0/3`, frames `0`, queue `-1`,
  queue `> 1<<22`, frames `> 1<<20`; валидный `192000 Hz / 1 ch / queue 1`;
* два независимых прогона одного сценария дают **побитово** одинаковый PCM и
  одинаковые `frames/underruns/overruns/device`;
* SDL: сначала `Auto` с заведомо несуществующим `SDL_AUDIODRIVER` должен
  дать null и причину в `device`; затем явный `SDL` — либо реальное
  устройство, либо `dummy`-драйвер, либо явный SKIP с текстом ошибки. В
  обоих успешных случаях проверяется структура (backend, start/pause/stop) и
  прогресс `Stats::frames` (callback действительно забрал кадры); точное
  число кадров не пинится, потому что оно зависит от таймингов устройства.

`ctest -R audio_output` проходит за ~0,12 с, метка `quick`.

## 6. Доверие и границы

Проверено: контракт ring/underrun/overrun, точная арифметика громкости,
пауза, повторный запуск, отсутствие аллокаций/блокировок в `pull`, сборка с
SDL3 3.4.18 и без него, реальный `Mixer` в роли источника, fallback без
устройства. SDL-путь на этой машине идёт через `dummy`-драйвер: он
подтверждает открытие/резюм/паузу/остановку и то, что callback действительно
забирает кадры (`frames > 0`), но **не** проверяет звук в наушниках и
задержку.

Не проверено: реальные устройства разных драйверов (pipewire/pulse/ALSA/
WASAPI), горячее подключение/смена устройства, дрейф часов, качество
ресемплинга при несовпадении частоты микшера и устройства, громкость в dB,
многоканальный вывод (> 2 каналов).

## 7. WSL2/headless

В этой WSL2 нет аудиоустройства: `SDL_InitSubSystem(SDL_INIT_AUDIO)`
возвращает `No available audio device`. Поэтому:

* `Backend::Null` — основной путь для CTest и `--headless`; он не требует
  SDL и не зависит от окружения;
* `Backend::Auto` в игре без устройства тихо уйдёт в null, а не упадёт;
* SDL-тест пытается открыть устройство, затем `dummy`; если недоступно и
  это, печатает `audio_output: SDL skip: <причина>` и завершается успешно.
  SKIP виден в stderr, а не маскируется.

Для ручной проверки реального звука на машине с устройством:
`SDL_AUDIODRIVER=pipewire ./build/sandbox/engine/tests/gm_audio_output_tests`
(или `pulseaudio`/`alsa`); вывод `audio_output: SDL ok (...)` означает, что
открылся реальный backend, `dummy driver` — что проверен только код-путь.

## 8. Что осталось

* Подключить `AudioOutput` к игровому циклу (`main.cpp` вне scope этой
  задачи): `open({mixer.sample_rate(), 2, …})`, `set_source` на `Mixer`,
  `start` после инициализации, `stop` при выходе.
* Свести `AudioOutput` и существующий `AudioDevice` (очередь байтов для
  `--music`) к одному API; сейчас это два независимых класса, и `main.cpp`
  по-прежнему использует `AudioDevice`.
* Выбор устройства/`SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK` из конфига,
  обработка hotplug и смены формата.
* Ресемплинг, если частота микшера не равна частоте устройства: сейчас
  контракт требует, чтобы `Mixer::init` шёл на `Config::sample_rate`;
  иначе SDL сделает свой resample, а не микшер.
* Настройка задержки (`frames`, `queue_frames`) под реальное устройство;
  текущие значения — безопасный дефолт, а не измеренный оптимум.
* Громкость в dB/кривая восприятия, отдельные bus'ы (music/SFX), mute.
* Запись/захват звука, многоканальный вывод, синхронизация с видео.
