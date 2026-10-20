# Инструменты и версии

Всё, чем снимались факты и собирается код. Репозиторий не хранит ни ассетов
игры, ни бинарников тулчейна.

## Игра

| | |
|---|---|
| установка | `/mnt/d/games/Gunmetal 3dfx` (`D:\games\Gunmetal 3dfx`) |
| версия | v1.36 (3dfx) |
| манифест | `docs/manifests/gunmetal-3dfx-136.sha256` (2161 файл, 373.7 MiB) |
| сводный SHA-256 | `b1d6e04321e011e8ea52a96df9c9cb738dcd8e950ae2465c339af880308601e4` |

## Python-тулкит

| | |
|---|---|
| Python | 3.14.4 (stdlib only, без pip) |
| тесты | `python3 regress.py` (113 проверок) / `--full` (121) |
| CLI | `python3 gm.py <command>` |

## Реверс

| | |
|---|---|
| дизассемблер | `objdump -D -b pei-i386 -M intel gundx.exe` → `/tmp/opencode/gundx.asm` |
| обёртки | `tools/re/disrange.py`, `tools/re/xref.py`, `tools/re/exelib.py` |
| карта адресов | `tools/re/names.csv` (90 записей) + `tools/re/names.py` |
| Ghidra | **не используется**: в системе нет Java, ставить JDK+Ghidra (~650 МБ) не понадобилось — карта собрана по objdump и проверена кодом |

Ghidra можно добавить позже (портативный JDK 21 + Ghidra в `~/tools`), когда
потребуется декомпилятор для `M5+`; `names.csv` уже совместим с любым
импортом (VA, имя, подсистема).

## C++ (M3)

| | |
|---|---|
| стандарт | C++20 |
| CMake | ≥ 3.25 (здесь 4.4.4) |
| сборщик | Ninja (здесь 1.13.2) |
| компилятор | Zig 0.17.0 (`zig cc` / `zig c++`, clang-подобный) в этой среде; MSVC/GCC/Clang — на обычных машинах |
| SDL3 | 3.4.18, статическая сборка, драйверы `dummy`/`offscreen` (без X11/Wayland) |

Подробности сборки — `docs/BUILD.md`.
