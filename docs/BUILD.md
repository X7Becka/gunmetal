# Сборка движка (M3)

Каркас: **C++20**, CMake ≥ 3.25, опционально **SDL3** (3.2+). Без SDL3
собирается headless-часть: `--dump` и `--headless` работают, окно — нет.

## Linux

```sh
sudo apt install build-essential cmake ninja-build libsdl3-dev
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

Если SDL3 нет: `cmake --preset release -DGM_WITH_SDL3=OFF` (или `-DSDL3_DIR=...`
на каталог с `SDL3Config.cmake`).

## Windows

MSVC 2022 + CMake, SDL3 через vcpkg (`vcpkg install sdl3`) или официальный
`SDL3-devel`:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Эта машина (WSL2, системного компилятора нет)

Тулчейн лежит вне репозитория, в `~/tools`:

| компонент | версия | путь |
|---|---|---|
| Zig (cc/c++) | 0.17.0 | `~/tools/zig-x86_64-linux-0.17.0/zig` |
| CMake | 4.4.4 | `~/tools/cmake-4.4.4-linux-x86_64/bin` |
| Ninja | 1.13.2 | `~/tools/bin/ninja` |
| SDL3 (static) | 3.4.18 | `~/tools/sdl3` |

Обёртки `~/tools/bin/zigcc` и `zigcxx` вызывают `zig cc` / `zig c++`. Полная
команда конфигурации:

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox -j
ctest --test-dir build/sandbox --output-on-failure
```

SDL3 собран как console-build (без X11/Wayland): доступны драйверы `dummy` и
`offscreen`. Проверка окна без дисплея:

```sh
SDL_VIDEODRIVER=dummy ./build/sandbox/engine/gunmetal --frames 3
```

## CLI

```
gunmetal [--game DIR] [--level NAME] [--dump FILE] [--headless]
         [--frames N] [--verbose] [--version]
```

* `--game` — корень установки (по умолчанию `$GUNMETAL_DIR` или
  `/mnt/d/games/Gunmetal 3dfx`).
* `--dump FILE` — размер и первые 64 байта (hex + ASCII); относительный путь
  ищется в `--game` без учёта регистра.
* `--headless` — без окна и SDL.
* `--frames N` — остановиться после N кадров.

## Что проверяет CTest

| тест | что проверяет |
|---|---|
| `core` | `resolve_ci`, арена, vec-математика, `rd16/24/32/64`, hex/ascii, разбор CLI, часы, headless-окно, уровни лога, BIGFILE-ридер (синтетический + реальный `sounds/SOU.IDX`: 999 записей), `.sbm`/`.bmp`/PNG (сверка FNV с Python на реальных текстурах) |
| `cli_version` | `gunmetal --version` |
| `cli_headless` | `--headless --frames 2` доходит до конца |
| `cli_dump` | дамп существующего файла печатает `size:` |
| `cli_dump` (вручную) | `--dump tex.sbm --png out.png` — валидный RGBA-PNG |
| `cli_dump_missing` | отсутствующий файл — код возврата ≠ 0 (не падение) |

Регрессия ассетов (`regress.py`) не зависит от C++-сборки и остаётся отдельной.
