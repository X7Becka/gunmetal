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
| дизассемблер | `objdump -D -M intel gundx.exe` → `~/.cache/gunmetal-re/gundx.asm` (+ `.idx`) |
| кэш реверса | `tools/re/gmcache.py` (`status`/`build`/`mirror`), `GUNMETAL_CACHE` |
| обёртки | `tools/re/disrange.py`, `tools/re/xref.py`, `tools/re/exelib.py` |
| карта адресов | `tools/re/names.csv` (119 записей) + `tools/re/names.py` |
| декомпилятор | **Ghidra 12.1.4 PUBLIC** + **pyghidra-mcp 0.2.7**, headless |
| Java | Temurin **21.0.12.1+1** (портативный, `~/tools`) |
| MCP | HTTP `http://127.0.0.1:8765/mcp`, конфиг — `opencode.json` |
| проект | `re/ghidra/` (не в git); текстовые выгрузки — `docs/re/ghidra/` |
| запуск | `tools/ghidra/start.sh` / `stop.sh`; см. `docs/GHIDRA.md` |

Ghidra импортирует `gundx.exe` с `ImageBase 0x400000`; проект и бинарники игры
в git не попадают. `names.csv` остаётся источником имён для MCP/экспорта.
Установка и сравнение MCP-вариантов — `docs/GHIDRA.md`.

## C++ (M3)

| | |
|---|---|
| стандарт | C++20 |
| CMake | ≥ 3.25 (здесь 4.4.4) |
| сборщик | Ninja (здесь 1.13.2) |
| компилятор | Zig 0.17.0 (`zig cc` / `zig c++`, clang-подобный) в этой среде; MSVC/GCC/Clang — на обычных машинах |
| SDL3 | 3.4.18, статическая сборка, драйверы `dummy`/`offscreen` (без X11/Wayland) |

Подробности сборки — `docs/BUILD.md`.

## Скорость и контекст

| | |
|---|---|
| кэш (листинг, индекс, exe, зеркало) | `~/.cache/gunmetal-re`, `tools/re/gmcache.py` |
| зеркало установки | `python3 tools/re/gmcache.py mirror --sync`, `regress.py --mirror` |
| индекс листинга | `gundx.asm.idx`, собирается `exelib` (bisect + seek) |
| срезы документации | `tools/ctx.py` (`doc`/`addr`/`find`/`outline`/`task`) |
| методика | `docs/RE_METHOD.md` |
