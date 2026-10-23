# PLAYER_GUIDE — как играть в переиздание

Статус: **честный черновик на 2026-10-23** (часть `M12.6`). Главное, что
надо знать сразу: **играбельного режима пока нет.** Есть C++-движок, который
читает все ресурсы оригинала, строит сцену, рендерит уровень и модели,
проигрывает видео и звук, но фронтенда, меню, брифингов, загрузки сейва из
UI и игрового цикла «сел → поехал → выстрелил» в нём ещё нет
(`CURRENT_STATE.md` §5, `ROADMAP.md` `M6.5`/`M8`/`M9`).

Ниже — только то, что действительно запускается. Все команды и вывод в этом
документе проверены на этой машине (Ubuntu 26.04 в WSL2, сборка
`build/sandbox`).

---

## 1. Что уже можно посмотреть

| что | команда | что видно |
|---|---|---|
| сводка уровня | `gunmetal --headless -l level0 --info` | мир, сущности, лайтмапы |
| сцена из скрипта | `gunmetal --scene level0` | свет, «места», предметы, двери, спавны |
| кадр уровня в PNG | `gunmetal -l level0 --render out.png` | софт-рендер с текстурами |
| окно с уровнем | `gunmetal -l level0` | интерактивный просмотрщик (нужен дисплей) |
| модель и анимация | `gunmetal --model …`, `--anim NAME --at MS` | `.sob`/`.soo` в PNG |
| видео | `gunmetal --play Intro --frames N` | `.sbv` (Intro, 500 кадров) |
| звук | `gunmetal --sample NAME`, `--sou1 NAME --wav OUT` | резолв имени, WAV |
| музыка | `gunmetal --song Dm1A` | плейлист `.sog` и записи `MUSIC/SOU.DAT` |
| файл игры | `gunmetal --dump FILE` | размер, заголовок, записи `.sbt` |

Подробности по каждой команде — `gunmetal --help` (полный список флагов).

---

## 2. Требования

* **Легальная установка Gunmetal 3dfx v1.36** (например
  `D:\games\Gunmetal 3dfx`, в WSL — `/mnt/d/games/Gunmetal 3dfx`). Ассетов в
  репозитории нет и не будет (`ROADMAP.md` `M12.7`); без установки движок
  отказывается работать (§4).
* Для сборки: C++20, CMake ≥ 3.25, Ninja; SDL3 опционален — без него
  собирается headless-часть (`--dump`, `--headless`, `--render`), окна нет
  (`docs/BUILD.md`).
* Для Python-тулкита (не обязателен для игры): Python 3.11+, только
  стандартная библиотека (`README.md`, «Требования»).

---

## 3. Сборка

### 3.1 Обычная Linux-машина

По `docs/BUILD.md` §Linux (в этой среде системного компилятора нет, команда
здесь **не проверялась**):

```sh
sudo apt install build-essential cmake ninja-build libsdl3-dev
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

Без SDL3: `cmake --preset release -DGM_WITH_SDL3=OFF`.

### 3.2 Эта машина (портативный тулчейн в `~/tools`)

Ровно эти команды выполнены здесь; конфигурация — код 0, сборка — код 0
(181 цель):

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
```

```
-- platform: SDL3 3.4.18
-- Configuring done (2.6s)
-- Generating done (0.1s)
-- Build files have been written to: /home/nooverload/build/gm-sub-moddocs/build/sandbox
```

```sh
cmake --build build/sandbox -j 8
```

```
[181/181] Linking CXX executable engine/tests/gm_weapons_tests
```

Проверка (полный набор — минуты; точечно быстрее):

```sh
ctest --test-dir build/sandbox -R "cli_version|cli_no_game" --output-on-failure
```

```
100% tests passed out of 2
```

Готовый бинарник: `build/sandbox/engine/gunmetal`.

### 3.3 Windows

MSVC 2022 + CMake, SDL3 через vcpkg — `docs/BUILD.md` §Windows. В этой
среде не проверялось; 64-bit/Windows-матрица — `M12.2` ⬜ в `ROADMAP.md`.

---

## 4. Запуск и указание установки

Порядок разрешения пути (первый выигрывает): `-g`/`--game DIR` →
`GUNMETAL_DIR` → `/mnt/d/games/Gunmetal 3dfx` (`docs/BUILD.md` §CLI).

```sh
./build/sandbox/engine/gunmetal -g "/mnt/d/games/Gunmetal 3dfx" --headless -l level0 --info
GUNMETAL_DIR="/mnt/d/games/Gunmetal 3dfx" ./build/sandbox/engine/gunmetal --headless -l level0 --info
```

Оба варианта дают одно и то же:

```
level: level0
world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
record types: 4:1 5:1 11:1 12:1 15:1 17:1 20:1 255:1
lightmaps: magic=SBC1 records=405 texels=51792
```

Если путь неверный, движок не падает, а объясняет, что делать (код 2):

```
$ ./build/sandbox/engine/gunmetal -g /nonexistent --headless
[error] game: directory not found: /nonexistent
hint: pass -g/--game DIR or set GUNMETAL_DIR; the directory must be the Gunmetal 3dfx install (gundx.exe and the data files)
```

---

## 5. Просмотр уровня

### 5.1 Сводка и сцена (без окна)

```
$ ./build/sandbox/engine/gunmetal --scene level0
scene: level0.sbt
lights: 29 (sky 0, local 29)
locations: 10 items: 2 sounds: 6 sprites: 4
doors: 25 switches: 24 entities: 350
things: 29 (absolute 6, models 29)
spawns: 8 (campaign 8, deathmatch 0)
```

Сцена строится исполнением скрипта уровня: те же счётчики сверены с
Python-оракулом на 36 уровнях (`docs/VM_SCENE.md`).

### 5.2 Кадр в PNG

```
$ ./build/sandbox/engine/gunmetal --headless -l level0 --render /tmp/opencode/mod/level0.png \
      --width 320 --height 200
bounds: (-3080, -408, -1920) .. (1616, 8836, 288)
render: 320x200
meshes: 192 faces: 1449
textures: 55 loaded, 0 missing
output: /tmp/opencode/mod/level0.png (256283 bytes)
```

Полезные флаги камеры и картинки (см. `--help`): `--pos X,Y,Z` + `--look
X,Y,Z`, `--clip N`, `--no-textures`, `--no-lightmaps`, `--wire`, `--things`,
`--third-person`, `--fov`, `--haze`. Без `--pos/--look` работает авто-кадр
(`docs/RENDER.md`); про камеру и спавн — `docs/CAMERA.md`, `docs/PLAYER.md`.

### 5.3 Окно

```sh
./build/sandbox/engine/gunmetal -l level0            # нужен дисплей (X11/Wayland/WSLg)
SDL_VIDEODRIVER=dummy ./build/sandbox/engine/gunmetal --frames 3   # без дисплея
```

Проверка без дисплея:

```
[info] video: window 640x400 driver=dummy
[info] run: game=/mnt/d/games/Gunmetal 3dfx
[info] run: frames=3 elapsed=0.0066183s fps=453.289
```

---

## 6. Модели, видео, звук

Модель (путь к `.sob` передаётся **как есть**; относительный ищется от
текущего каталога):

```
$ ./build/sandbox/engine/gunmetal --model "/mnt/d/games/Gunmetal 3dfx/SOBS/kingtest.sob" \
      --render /tmp/opencode/mod/model.png --width 320 --height 240
bounds: (-551, -585, -470) .. (1179, 140, 705)
render: 320x240
meshes: 1 faces: 310
textures: 21 loaded, 0 missing
output: /tmp/opencode/mod/model.png (307528 bytes)
```

Анимация `.soo` (имя резолвится в `Sobanim/` самой установки):

```
$ ./build/sandbox/engine/gunmetal --anim Cultist --at 1000 --width 240 --height 240 \
      --render /tmp/opencode/mod/pose.png
[info] anim: mode=1 frame=1->2 alpha=0.273333 at=1000ms
bounds: (-31, -4, -38) .. (31, 5, 23)
render: 240x240
meshes: 1 faces: 46
textures: 1 loaded, 0 missing
output: /tmp/opencode/mod/pose.png (230723 bytes)
```

Видео (`--frames` ограничивает число кадров):

```
$ ./build/sandbox/engine/gunmetal --headless --play Intro --frames 3
[info] video: clip=Intro.sbv 320x160 frames=500
[info] run: frames=3 clip frames=500
```

Звук — резолв имени как в движке (сначала `sounds`, потом `music`):

```
$ ./build/sandbox/engine/gunmetal --sample alarmk
sample alarmk: archive=sounds name=ALARMK offset=24369 size=13619
$ ./build/sandbox/engine/gunmetal --sample DM1A01
sample DM1A01: archive=music name=DM1A01 offset=0 size=31148
$ ./build/sandbox/engine/gunmetal --sample nosuchname
sample nosuchname: not found (the original plays silence)
```

Декод в WAV (SOU1 — ADPCM, SOU2 — «формат 4»):

```
$ ./build/sandbox/engine/gunmetal --sou1 alarmk --wav /tmp/opencode/mod/alarmk.wav
sou1: ALARMK
format: 16000 Hz, 1 ch, 1.700 s
wav: /tmp/opencode/mod/alarmk.wav (54452 bytes)
```

Музыкальный плейлист:

```
$ ./build/sandbox/engine/gunmetal --song Dm1A
song: Dm1A.sog
tracks: 8
  Dm1A01 31148 bytes
  Dm1A02 23716 bytes
  ...
```

Файлы игры (что внутри `.sbt`) — фрагмент вывода:

```
$ ./build/sandbox/engine/gunmetal --dump "/mnt/d/games/Gunmetal 3dfx/level0.sbt"
...
kind: SBT level
version: 0
records: 8
...
script: things=0 handles=350 code=62497 flag=0
```

---

## 7. Логи

* `GM_LOG=trace|debug|info|warn|error|off` — уровень лога движка
  (`ROADMAP.md` `M12.5`). По умолчанию печатаются строки `[info] run: …`:

  ```
  $ GM_LOG=debug ./build/sandbox/engine/gunmetal --headless -l level0 --frames 2
  [info] run: game=/mnt/d/games/Gunmetal 3dfx headless level=level0
  [info] run: frames=2 elapsed=0.29603s fps=6.75608
  ```

* `-v`/`--verbose` — подробный лог (в проверенных путях вывода не добавил).
* Кэши инструментов (листинг, зеркало, декомпиляции) — `~/.cache/gunmetal-re`,
  их устройство и инвалидация: `docs/LAYOUT.md` §4. Это кэш **реверс**-
  инструментов, не движка.

---

## 8. Известные ограничения

* **Нет фронтенда и игрового цикла.** Ни меню, ни брифингов, ни кампании,
  ни DM, ни сети, ни загрузки сейва из UI (`CURRENT_STATE.md` §5;
  `ROADMAP.md` `M6.5`, `M8`, `M9`, `M10`).
* **Нет 60 FPS.** Измерен только «холодный» прогон: загрузка + один кадр +
  PNG — 182 мс на зеркале для 320×200 (`docs/PERFORMANCE.md` §3–§4).
  Прогретого цикла с рендером пока нет — FPS в логах выше (`frames=3
  elapsed=… fps=…`) включает загрузку и к первому кадру не относится.
* **Картинка не совпадает с оригиналом попиксельно.** Открытые пункты:
  текстуры/UV (`M4.1.8`), кокпит в кадре (`M4.4.5`), фог; список —
  `CURRENT_STATE.md` §5, детали — `docs/TEXUV.md`, `docs/ALPHA.md`,
  `docs/FOG.md`, `docs/FRAME_METRIC.md`.
* **Звук**: декодеры SOU0/1/2 и микшер есть, вывод — SDL3 или `null`
  (`docs/AUDIO_OUTPUT.md`). Если воспроизведение не слышно, проверяйте декод
  в WAV (§6) — он не зависит от аудиоустройства.
* **Windows/64-bit** — `M12.2` ⬜; на этой машине проверена только Linux-
  сборка.

---

## 9. Куда смотреть дальше

| вопрос | документ |
|---|---|
| что вообще готово и что нет | `CURRENT_STATE.md` §4–§5 |
| план и статусы пунктов | `ROADMAP.md` |
| форматы и рендер | `docs/FORMATS.md`, `docs/RENDER.md` |
| метрика сравнения кадров | `docs/FRAME_METRIC.md` |
| производительность | `docs/PERFORMANCE.md` |
| как собрать и что проверяет CTest | `docs/BUILD.md` |
| моддинг скриптов уровней | `docs/MODDING.md` |
| частые вопросы | `docs/FAQ.md` |
