# Ghidra: выбор, установка и рабочий процесс

**Статус:** Ghidra 12.1.4 + pyghidra-mcp 0.2.7 работают headless в WSL2.
Проект — `re/ghidra/` (не в git), текстовые выгрузки — `docs/re/ghidra/`.

## 1. Почему не GUI-плагины

| вариант | режим | плюсы | минусы | вердикт |
|---|---|---|---|---|
| LaurieWired/GhidraMCP | Ghidra GUI + HTTP plugin | самый известный, простой | нужен GUI/X11, headless-режима нет | не для WSL |
| GhidrAssistMCP | Ghidra GUI extension | 100 tools, ресурсы, prompts | GUI, Java 25 | не для WSL |
| bethington/ghidra-mcp | GUI plugin + headless server + bridge | 209 tools, create/structs/debugger, production | extension + bridge + Maven/headless jar, больше движущихся частей | запасной вариант |
| mrphrazer/ghidra-headless-mcp | pyghidra, 212 tools | headless, transactions, scripts | молодой проект, «vibe coded» | близкий второй |
| **clearbluejar/pyghidra-mcp** | **pyghidra, headless-first** | **stdio/HTTP, проекты, semantic search, CLI, Apache-2.0, зрелые тесты** | **15 tools, нет create_function/structs** | **выбран** |

Для этой задачи важнее не «200 инструментов», а стабильный headless-процесс,
повторяемый проект и возможность ходить в него из OpenCode. PyGhidra-MCP даёт
именно это; недостающие операции (создание функций, кастомные скрипты) можно
добавить отдельным pyghidra-скриптом, не меняя MCP-слой. Bethington имеет
`create_function` из коробки, но требует GUI-extension + bridge + headless jar;
его имеет смысл подключить вторым сервером, если понадобится struct/debugger.

## 2. Установка (без root)

| компонент | версия | путь |
|---|---|---|
| Temurin JDK | 21.0.12.1+1 | `~/tools/jdk-21.0.12.1+1` |
| Ghidra | 12.1.4 PUBLIC (`20260921`) | `~/tools/ghidra_12.1.4_PUBLIC` |
| uv | 0.13.3 | `~/tools/bin/uv` |
| Python для MCP | 3.13.16 (uv-managed) | `~/.local/share/uv/python/...` |
| pyghidra-mcp | 0.2.7 | `~/.local/bin/pyghidra-mcp` |

Ghidra распакована из официального zip. **Важно:** Python `zipfile` не сохраняет
Unix-биты, поэтому `Ghidra/Features/Decompiler/os/linux_x86_64/decompile` и
`sleigh` надо `chmod +x`; иначе декомпилятор молча возвращает пустой `code`.
Это уже исправлено в установке; при переустановке повторить:

```bash
chmod +x ~/tools/ghidra_12.1.4_PUBLIC/ghidraRun \
         ~/tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless
find ~/tools/ghidra_12.1.4_PUBLIC/Ghidra -path '*/os/linux_x86_64/*' \
     -type f -exec chmod +x {} +
```

## 3. Запуск

```bash
cd ~/gunmetal-re
./tools/ghidra/start.sh      # headless HTTP на 127.0.0.1:8765
opencode mcp list --print-logs  # ghidra connected
./tools/ghidra/stop.sh
```

`start.sh` запускает сервер через `setsid`: без этого оболочка агента/инструмента
убивает процесс при завершении команды (симптом — `ConnectionRefused` в
`opencode.log` при живом проекте). Первый запуск импортирует `gundx.exe` и
делает auto-analysis (≈1 мин на этой машине), затем проект открывается из
`re/ghidra/`. Сервер слушает только
loopback; `opencode.json` подключает его как remote MCP без OAuth.

Конфиг добавлен и в проект (`gunmetal-re/opencode.json`), и глобально
(`~/.config/opencode/opencode.json`): текущая сессия OpenCode может стартовать
из `/mnt/c/Users/nooverload`, где проектный конфиг не виден. Для проектного
конфига нужен запуск OpenCode из `gunmetal-re`; глобальный работает всегда.

Бинарник в проекте называется **`/gundx.exe-902b77`**. Это имя нужно передавать
в `binary_name` инструментов.

## 4. Инструменты, которыми пользуемся

- `decompile_function(binary, address|name, include_xrefs/strings/callees)` —
  псевдо-C; принимает и список адресов.
- `list_xrefs`, `gen_callgraph` — кто вызывает и кого вызывает.
- `disassemble` — точные инструкции, когда декомпилятор врёт на x87.
- `search_symbols_by_name`, `search_strings`, `search_code` (semantic/literal).
- `rename_function`, `rename_variable`, `set_variable_type`,
  `set_function_prototype`, `set_comment` — аннотирование проекта.
- `read_bytes`, `list_imports`, `list_exports`, `metadata`.

Проверено на пилоте:

```text
0x477f90  weapon_target_scan   обход things по range/spread
0x473a00  thing_target_scan    обход 0x7ac3e0/0x81c244, ближайшая цель
0x41e025  camera_basis         Rz(-yaw)*Rx(-pitch)*Ry(roll)
0x433e00  soo_player           проигрыватель .soo
0x4795b0  event_post           очередь событий, не спавнер снаряда
0x4799b0  event_dispatch       разбор очереди по типу записи
0x479960  generate_event       GENERATE: дверь + подскрипт
0x45fa00  generate_runner      id -> подскрипт сущности
```

Выгрузки — `docs/re/ghidra/*.c`, `index.json`, `metadata.json`; команда:

```bash
~/.local/share/uv/tools/pyghidra-mcp/bin/python tools/ghidra/export.py
```

## 4.1. CLI без зависимостей: `tools/ghidra/mcp.py`

Агентские оболочки не всегда имеют пакет `mcp` (он живёт только в
venv `pyghidra-mcp`), поэтому в проект добавлен CLI на голом `urllib` —
он говорит с тем же сервером по streamable-HTTP JSON-RPC:

```sh
python3 tools/ghidra/mcp.py tools                 # список MCP-инструментов
python3 tools/ghidra/mcp.py decompile 0x444476    # псевдо-C (в т.ч. внутрь функции)
python3 tools/ghidra/mcp.py decompile 0x451c20 0x452020   # пачкой
python3 tools/ghidra/mcp.py disasm 0x4444c0 --count 40 --bytes
python3 tools/ghidra/mcp.py xrefs 0x81c290        # кто ссылается на адрес
python3 tools/ghidra/mcp.py strings PLAYER_ --limit 50
python3 tools/ghidra/mcp.py search "player armor" --limit 3
python3 tools/ghidra/mcp.py bytes 0x4b53e8 --size 64
python3 tools/ghidra/mcp.py callgraph 0x451c20 --direction called
python3 tools/ghidra/mcp.py raw list_xrefs '{"name_or_address":"0x81bd00"}'
```

Пакетный режим держит одну MCP-сессию на файл запросов (так быстрее и не
плодит сессии в логе сервера):

```sh
cat > /tmp/opencode/req.jsonl <<'EOF'
{"tool":"decompile_function","args":{"name_or_address":"0x451c20"},"label":"status_load"}
{"tool":"list_xrefs","args":{"name_or_address":"0x81c290"}}
EOF
python3 tools/ghidra/mcp.py batch /tmp/opencode/req.jsonl
```

В Code Mode этого окружения те же операции доступны **нативно** —
`tools.ghidra.*`: `read_bytes`, `search_strings`, `list_exports`/`list_imports`,
`list_project_binary_metadata`, `rename_function`, `rename_variable`,
`set_comment`, `set_function_prototype`, `import_binary`, `save`. Это тот же
MCP-сервер, без запуска CLI: удобно для серии правок проекта (имена,
комментарии, прототипы), но правила прежние — правки делает основной агент,
а вывод считается гипотезой до проверки данными/тестом. Для пакетных
чтений по-прежнему дешевле `mcp.py batch`.

Настройки: `GM_GHIDRA_URL` (по умолчанию `http://127.0.0.1:8765/mcp`),
`GM_GHIDRA_BINARY` (по умолчанию `/gundx.exe-902b77`). Если сервер не поднят,
ошибка прямо говорит запустить `tools/ghidra/start.sh`. Инструменты,
которые меняют проект (`rename_function`, `set_comment`, …), доступны через
`raw`/`batch`; помни, что проект общий для всех сабагентов — переименования
делает только основной агент.

## 5. Правила доказательности

1. Декомпилятор — **инструмент гипотез**, не доказательство. x87, union,
   indirect calls и ручной ассемблер часто дают неверные типы.
2. Любой вывод проверяется `tools/re/emu.py`, байтовым кодеком или golden-тестом.
3. Имена в `tools/re/names.csv` получают `status=гипотеза`, пока нет xref +
   теста/эмуляции; только потом `код`.
4. Проект Ghidra и бинарники игры в git не попадают; в git — только текстовые
   выгрузки и скрипты.

## 5.1. Как спрашивать Ghidra дёшево

- Сначала `xrefs`/`strings`/`bytes` (дешёвые), потом `decompile` — и только
  для адреса, который уже нужен по гипотезе. Не «декомпилируй всё».
- Несколько адресов — одним `batch`, а не N запусками (одна сессия, меньше
  токенов на заголовки и повторные ответы).
- Ответ используй как гипотезу: в доки идут адрес + наблюдение на данных +
  проверка (эмулятор/golden), а не текст декомпилятора. Clean-room: не копируй
  псевдо-C в `engine/`.
- Проект общий: переименования/комментарии — только основной агент; сабагент
  спрашивает и записывает вывод в свой отчёт.
- Если ответ не нужен целиком, проси конкретное: `--count`, `--limit`,
  диапазон адресов; полный дамп функции редко стоит своих токенов.

Общая методика и бюджет контекста — `docs/RE_METHOD.md` §5–6.

## 6. Ограничения текущего выбора

- Нет `create_function`/struct-editor tools: функции, которые Ghidra не создала,
  приходится заводить pyghidra-скриптом или disassemble-эвристикой.
- Нет `run_script` в MCP: кастомные Ghidra-скрипты запускаются отдельным
  процессом (проект должен быть закрыт).
- Semantic-индекс (`ChromaDB`, `all-MiniLM`) грузится фоном; первые
  `search_code` могут ждать индексации.
- Реальный GUI/отладчик не используются.

Если понадобится `create_function`, structs или debugger — второй сервер
`bethington/ghidra-mcp` можно поставить рядом на другом порту, не меняя этот.
