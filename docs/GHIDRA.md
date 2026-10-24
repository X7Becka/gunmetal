# Ghidra: выбор, установка и рабочий процесс

**Статус:** Ghidra 12.1.4 + pyghidra-mcp 0.2.7 работают headless в WSL2.
Проект — `re/ghidra/` (не в git), текстовые выгрузки — `docs/re/ghidra/`.
В проекте три структуры из карт полей (`gm_thing`, `gm_thing_type`,
`gm_player`) и типизированные массивы `things[260]`, `thing_types[128]`,
`players[8]`; кэш декомпилятора — 232 записи, 0 устаревших.

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

### 3.1. Проект должен жить в `re/ghidra`, не в worktree

Ghidra-проект — это **состояние**, а не расходник: имена, комментарии и типы
копятся только в нём. Правило: `--project-path` всегда `$ROOT/re/ghidra`;
`GM_GHIDRA_PROJECT` не должен указывать в `/tmp` или `/var/tmp` (worktree
удаляется, проект умирает вместе с ним).

Диагностика потери (наблюдалось 2026-10-22):

```sh
ls -l /proc/$(pgrep -f pyghidra-mcp)/cwd     # -> /var/tmp/gm-sub-... (deleted)
python3 tools/ghidra/mcp.py symbols "weapon_target"   # -> пусто
```

Симптом: `rename_function` проходит, но `save` отвечает
`java.io.IOException: File error during save` — каталог проекта удалён,
изменения остаются только в памяти и исчезнут с перезапуском. Лечение:
остановить сервер, `cp -a re/ghidra re/ghidra.bak-$(date +%F)`, запустить
`tools/ghidra/start.sh` из корня репозитория и проверить
`mcp.py metadata` (тот же SHA-256 exe) — импорт и анализ при живом проекте
не перезапускаются.

`start.sh` теперь это проверяет сам: отказывается стартовать с
`/tmp`/`/var/tmp`-путём (`GM_GHIDRA_ALLOW_EPHEMERAL=1` — осознанный обход),
не поднимает второй сервер на занятом порту (печатает pid и путь проекта
занявшего) и перед запуском складывает снапшот проекта в
`re/ghidra-backups/<дата>` (хранится 3 последних; каталог в `.gitignore`).


Имена и статусы восстанавливаются одной командой (идемпотентно):

```sh
python3 tools/ghidra/apply_names.py --dry-run   # план
python3 tools/ghidra/apply_names.py             # rename + plate-комментарий + save
```

`names.csv` — единственный источник имён; статус (`код`/`гипотеза`) и
доказательство попадают в комментарий функции, поэтому проект сам показывает,
что проверено, а что нет. Данные/таблицы (`kind=table/data`) не
переименовываются — это не функции.

Проверено 2026-10-24 на `re/ghidra`: проект открылся без анализа (1506
функций), `apply_names.py --check --json` отвечает `0 missing` для 105
code-имён (ещё 6 адресов помечены `kind=label`, 23 — data, 2 — table),
plate-комментарии на месте, `save` отработал. После **повторного** рестарта
имена и комментарии сохранились. Отдельный `createFunction`-проход не нужен:
из 1303 call-целей `.text` только 21 без начала функции, и все внутри
существующих (хвостовые вызовы/CRT). `names.csv` — карта адресов, а не
функций, поэтому внутренние инструкции без границы функции теперь имеют
`kind=label` и не участвуют в переименовании.

После успешного `save` `apply_names.py` **инвалидирует кэш**:
удаляются записи переименованных адресов и все записи, которые их вызывают
(иначе старое псевдо-C продолжает показывать `FUN_…` и не содержит
plate-комментария). Проверка дрейфа — `apply_names.py --check --json`;
`state.py --check` вызывает её и падает, если имена не применены.


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
`tools.ghidra.*`: `decompile_function`, `disassemble`, `list_xrefs`,
`gen_callgraph`, `search_code`, `search_symbols_by_name`, `search_strings`,
`read_bytes`, `list_exports`/`list_imports`, `list_project_binaries`,
`list_project_binary_metadata`, `rename_function`, `rename_variable`,
`set_variable_type`, `set_comment`, `set_function_prototype`, `import_binary`,
`save` (проверено: `read_bytes 0x4752d0` совпал с листингом). Это тот же
MCP-сервер, без запуска CLI: удобно для серии правок проекта (имена,
комментарии, прототипы), но правила прежние — правки делает основной агент,
а вывод считается гипотезой до проверки данными/тестом. Для пакетных
чтений по-прежнему дешевле `mcp.py batch`.

`mcp.py decompile` для числовых адресов **сам читает и пишет `fncache`**
(если не запрошены `--xrefs`/`--strings`/`--callees`). Нативные
`tools.ghidra.decompile_function` кэш обходят: для одного адреса это
приемлемо, для серии — `mcp.py decompile <addr>...` или `batch`. Флаги:
`--refresh` (перечитать и перезаписать), `--no-cache` (обойти кэш).

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

## 5.2. Кэш декомпилятора: `tools/ghidra/fncache.py`

Замер по истории проекта: **745 запросов декомпиляции на 343 разных адреса**
(53 % — повторные), при этом результат нигде не сохранялся и умирал вместе с
чатом. Кэш закрывает это: адрес → псевдо-C, ключ — SHA-256 exe.

```sh
python3 tools/ghidra/fncache.py get 0x4752d0            # из кэша или сети
python3 tools/ghidra/fncache.py get 0x457390 --head 40  # большой функции — срез
python3 tools/ghidra/fncache.py warm                    # все адреса names.csv
python3 tools/ghidra/fncache.py warm --file tools/ghidra/hot_addrs.txt
python3 tools/ghidra/fncache.py warm --cached           # перечитать весь кэш
python3 tools/ghidra/fncache.py stats --json
python3 tools/ghidra/fncache.py stale                   # что устарело и почему
python3 tools/ghidra/fncache.py selftest                # без сервера
python3 tools/ghidra/fncache.py export docs/re/ghidra/fn
```

- Кэш: `~/.cache/gunmetal-re/ghidra/<sha12>/<addr>.json` (переживает
  `wsl --shutdown`, потерю worktree и перезапуск проекта Ghidra). Попадание
  работает при выключенном MCP-сервере.
- Таблицы и данные (`kind=table/data` в `names.csv`) пропускаются: они не
  функции, декомпилятор отвечает «not found».
- `--head N` обязателен для гигантов: `0x457390` (диспетчер ВМ) — 3014 строк,
  ~120 КБ псевдо-C. Полный текст в контекст не тянем.
- Псевдо-C в кэше — по-прежнему **гипотеза**: доказательство — байты, данные,
  эмулятор или golden-тест (см. §5).
- **Свежесть.** `stale_report` считает записи, которые разошлись с проектом:
  своё имя всё ещё `FUN_`, вызывают уже именованную функцию или не имеют
  plate-комментария. `apply_names.py` чистит такие записи автоматически;
  после смены типов `apply_types.py` чистит кэш целиком (структуры меняют
  почти каждую декомпиляцию). Ручное лечение:
  `fncache.py warm --cached` (перечитать всё) или
  `fncache.py warm --refresh --names tools/re/names.csv`.
- Догрев по самым упоминаемым в доках адресам и по истории декомпиляций
  (на 2026-10-24: 3424 адреса в `.text`, 232 записи / 1409 КиБ, 0 устаревших;
  `tools/ghidra/hot_addrs.txt` — 50 самых частых function-start адресов из
  базы сессий):

```sh
python3 - <<'PY' > /tmp/opencode/top.txt
import re,glob,collections
c=collections.Counter()
for p in glob.glob('docs/**/*.md',recursive=True)+['NOTES.md','FORMATS.md']:
    for a in re.findall(r'0x[0-9a-f]{6,8}',open(p,encoding='utf-8').read()):
        v=int(a,16)
        if 0x401000<=v<0x4a1000: c[v]+=1
print(' '.join(f'0x{a:x}' for a,_ in c.most_common(60)))
PY
python3 tools/ghidra/fncache.py get --no-headers --head 1 $(cat /tmp/opencode/top.txt)
```


## 5.3. Структуры из карт полей

Карты полей уже есть в `docs/THING_FIELDS.md` §3/§4 и
`docs/PLAYER_FIELDS.md` §3, но Ghidra их не знала: декомпилятор показывал
`*(int *)(&DAT_007ac3e0 + i * 0x14a)`. Два скрипта закрывают разрыв.

```sh
python3 tools/ghidra/typegen.py            # docs -> tools/ghidra/gm_types.h
python3 tools/ghidra/typegen.py --check    # падает, если header устарел
```

`typegen.py` (stdlib) разбирает таблицы, раскладывает поля по смещениям,
заполняет неизвестные диапазоны байтами и **проверяет размеры**: 0x528
(`gm_thing`), 0x3a8 (`gm_thing_type`), 0x3510 (`gm_player`). Ошибка в
смещении/типе — отказ, а не догадка.

```sh
tools/ghidra/stop.sh                       # проект должен быть закрыт
tools/ghidra/apply_types.sh --dry-run      # план
tools/ghidra/apply_types.sh                # импорт + массивы + save
tools/ghidra/start.sh
```

`apply_types.py` (pyghidra/JVM) импортирует header в Data Type Manager и
типизирует базы из карт:

| адрес | тип | символ |
|---|---|---|
| `0x7ac080` | `gm_thing[260]` | `things` |
| `0x776d60` | `gm_thing_type[128]` | `thing_types` |
| `0x819040` | `gm_player[8]` | `players` |

Скрипт идемпотентен, отказывается чистить диапазон, если там есть функции,
и восстанавливает ярлыки документированных адресов (`0x7ac3e0`,
`0x81bd00`, …) после `clearCodeUnits`. Успешный прогон очищает `fncache`
(структуры меняют псевдо-C) и пишет отчёт
`docs/re/ghidra/types_report.md`: для пяти горячих функций — сколько было
`&DAT_…`-обращений и сколько стало обращений к полям.

Результат 2026-10-24:

| функция | `&DAT_` до | после | поля |
|---|---:|---:|---:|
| `0x443690` camera_spring | 395 | 0 | 145 |
| `0x470500` damage_thing | 52 | 0 | 34 |
| `0x470c70` thing_tick | 344 | 0 | 61 |
| `0x4752d0` projectile_tick | 62 | 0 | 10 |
| `0x47a980` chassis_spawn | 141 | 0 | 24 |

Пример: `things[param_1].flags_b`, `things[param_1].expire_ticks`,
`players[param_1].pose`. Имена полей — из карт (статусы `proven/hypothesis`
там же); типизация не делает псевдо-C доказательством.

## 6. Ограничения текущего выбора

- Нет `create_function`/struct-editor tools: структуры импортируются
  офлайн через `apply_types.py` (проект закрыт), а не через MCP.
- Нет `run_script` в MCP: кастомные Ghidra-скрипты запускаются отдельным
  процессом (проект должен быть закрыт).
- Semantic-индекс (`ChromaDB`, `all-MiniLM`) грузится фоном; первые
  `search_code` могут ждать индексации.
- Реальный GUI/отладчик не используются.

Если понадобится `create_function`, structs или debugger — второй сервер
`bethington/ghidra-mcp` можно поставить рядом на другом порту, не меняя этот.
