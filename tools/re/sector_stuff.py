#!/usr/bin/env python3
"""``SECTOR_STUFF``: какой слот берёт команда (опкоды 27-31 и 40-45).

Вопрос: ``SET_SOURCESECTOR_*`` (27-31) и ``SET_HANDLESECTOR_*`` (40-45)
пишут массив ``0x800b60`` (256 x ``0x58``) и/или запись сектора мира
(``0x5670d8``, шаг ``0x18``), но **индекс слота не в операндах**.  Этот
инструмент отвечает на него по дизассемблеру и по данным 36 уровней.

Доказанная цепочка (все адреса — VA ``gundx.exe`` 1.36)::

    # источник (edi) — третий аргумент командной ВМ
    4573ab   mov edi,[ebp+0x10]      ; edi = arg3 = "сектор источника"
    457396   sub esp,0x74            ; ...
    457305   mov ecx,[esi+0x5e3df0]  ; тикер: PC спящего/активного слота
    45734b   mov edx,[esi+0x616258]  ; ... и его сектор -> arg3 (push 0x457353)
    4575bb   push edi                ; GENERATE (2): edi родителя -> arg2 0x479960
    479993   ...                     ; 0x479960(id, sector, ...) -> 0x45fa00
    45fac2   push edx                ; 0x45fa00 arg2 -> arg3 0x457390
    45c4a5   mov [ecx*4+0x616258],edi; SLEEP сохраняет arg3 в слоте 0..255

    # хэндл (E-операнд) — таблица SECTOR_HANDLE
    4582bc   mov eax,[ecx*4+0x617fb8]; 40: sector = sectors[handle_row]
    4582c6   lea edi,[eax+eax*4]     ; slot = sector*11
    4582c9   lea eax,[eax+edi*2]     ;      *11
    4582cc   shl eax,0x3             ;      *8 -> 0x58
    463900   fread(count, tags, sectors)  ; SECTOR_HANDLE (тип 17) читается
                                          ; из .sbt: 0x5e3de0/0x5b1df8/0x617fb8

Слот ``SECTOR_STUFF`` = ``index*11*8`` = ``index*0x58``; запись сектора мира
= ``0x5670d8 + index*0x18``.

Что инструмент делает по данным (без Ghidra, только ``.sbt``/``.sbw``):

* находит все команды 27-31/40-45 в ``SCRIPT`` (тип 11) каждого уровня;
* для 40-45 разрешает E-операнд через таблицу ``SECTOR_HANDLE`` (тип 17) —
  это **доказанный** статический слот (в поставке таблица не пуста: 33/36
  уровней, вопреки старой записи в ``docs/RECORDS.md`` §2.5);
* для 27-31 статического индекса в файле нет; инструмент считает
  *возможные* секторы источника по графу запусков: корень — главный скрипт
  (``offset 0``; его источник = 0 — гипотеза, см. :func:`source_origins`) и
  события секторов
  (``SET_SECTOR_EVENTS`` — литеральный слот + два id события), рёбра —
  ``GENERATE`` (родитель передаёт свой сектор, ``0x4575bb``).  Пишет
  ``static_sectors`` (доказанно возможные), ``static_complete`` (все ли
  маршруты объяснены) и ``block_origin``.

Использование::

    python3 tools/re/sector_stuff.py                 # сводка по 36 уровням
    python3 tools/re/sector_stuff.py --summary
    python3 tools/re/sector_stuff.py --level level0   # все команды уровня
    python3 tools/re/sector_stuff.py --map            # таблица опкодов
    python3 tools/re/sector_stuff.py --json > ss.json
    python3 tools/re/sector_stuff.py --self-test

``--json`` побайтно детерминирован (фиксированный порядок уровней, блоков и
команд; ``sort_keys``).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm                  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")

SCRIPT_RECORD = 11
SECTOR_HANDLE_RECORD = 17
SECTOR_STUFF_RECORD = 8

#: база и шаг рантайм-массива ``SECTOR_STUFF`` (``docs/RECORDS.md`` §2)
SECTOR_STUFF_BASE = 0x800B60
SECTOR_STUFF_STRIDE = 0x58
SECTOR_STUFF_SLOTS = 256

#: опкод -> всё, что доказано дизасмом.  ``slot``:
#:   ``source`` — индекс = сектор источника, в операндах его нет;
#:   ``handle`` — индекс = номер сектора из строки таблицы ``SECTOR_HANDLE``.
COMMANDS = {
    27: {
        "name": "SET_SOURCESECTOR_MOVEMENT",
        "sig": "LLLAAA",
        "handler": 0x457FC6,
        "slot": "source",
        "target": "sector_stuff",
        "store": ("0x457fef", "0x457fff", "0x45800f",
                  "0x458019", "0x458023", "0x458032",
                  "0x45803c", "0x45804b", "0x458055"),
        "slot_math": ("0x457fdd", "0x457fe9", "0x457fec"),
        "bound": "0x457fc6 (test edi,edi) / 0x457fd4 (cmp edi,0x100)",
        "oob": "0x45899a: молча пропускает 0x24 байта",
        "fields": (("+0x00", "move[0]", "f64 = fild(L0)", "0x457fef"),
                   ("+0x08", "move[1]", "f64 = fild(L1)", "0x457fff"),
                   ("+0x10", "move[2]", "f64 = fild(L2)", "0x45800f"),
                   ("+0x18", "extra[0]", "8 сырых байт A0", "0x458019"),
                   ("+0x20", "extra[1]", "8 сырых байт A1", "0x458032"),
                   ("+0x28", "extra[2]", "8 сырых байт A2", "0x45804b")),
    },
    28: {
        "name": "SET_SOURCESECTOR_BRIGHTNESS",
        "sig": "L",
        "handler": 0x458063,
        "slot": "source",
        "target": "world_sector",
        "store": ("0x45807c",),
        "slot_math": ("0x458064",),
        "bound": "0x42d8a5: 0 <= i < [0x5670dc] (иначе NULL -> 0x45808d)",
        "oob": "0x45808d: 'unable to obtain sector %ld' (0x4b85cc), "
               "операнд съедается",
        "fields": (("+0x16", "light", "u8 = L & 0xff", "0x45807c"),),
    },
    29: {
        "name": "SET_SOURCESECTOR_FLAGS",
        "sig": "L",
        "handler": 0x458111,
        "slot": "source",
        "target": "sector_stuff",
        "store": ("0x458134",),
        "slot_math": ("0x458128", "0x45812e"),
        "bound": "0x458111 (test edi,edi) / 0x458119 (cmp edi,0x100)",
        "oob": "0x45c36a: молча пропускает 4 байта",
        "fields": (("+0x50", "flags", "u32", "0x458134"),),
    },
    30: {
        "name": "SET_SOURCESECTOR_TYPE",
        "sig": "L",
        "handler": 0x4580A9,
        "slot": "source",
        "target": "world_sector",
        "store": ("0x42d9ff",),
        "slot_math": ("0x4580c2",),
        "bound": "0x4580a9 (test edi,edi) / 0x4580b1 (cmp edi,0x100); "
                 "0x42d9d5 ещё раз 0 <= i < [0x5670dc]",
        "oob": "0x45c36a (молча) либо 0x42d9d5 -> -1",
        "fields": (("+0x14", "kind", "u16 = value*6", "0x42d9ff"),),
    },
    31: {
        "name": "SET_SOURCESECTOR_DAMAGE",
        "sig": "L",
        "handler": 0x4580CF,
        "slot": "source",
        "target": "sector_stuff",
        "store": ("0x458100", "0x458106"),
        "slot_math": ("0x4580ec", "0x4580f7", "0x4580fa"),
        "bound": "0x4580d5 (test edi,edi) / 0x4580e0 (cmp edi,0x100)",
        "oob": "0x45c41b: молча (операнд уже прочитан)",
        "fields": (("+0x40", "damage_a", "u32 = v & 0xffff", "0x458100"),
                   ("+0x44", "damage_b", "i32 = v >> 16 (арифм.)", "0x458106")),
    },
    40: {
        "name": "SET_HANDLESECTOR_MOVEMENT",
        "sig": "ELLLAAA",
        "handler": 0x45829B,
        "slot": "handle",
        "target": "sector_stuff",
        "store": ("0x4582cf", "0x4582d9", "0x4582e3",
                  "0x4582ed", "0x4582f7", "0x458301",
                  "0x45830b", "0x458315", "0x45831f"),
        "slot_math": ("0x4582bc", "0x4582c6", "0x4582c9", "0x4582cc"),
        "bound": "нет: sector = [0x617fb8 + i*4] пишется как есть",
        "oob": "проверки нет (0x4582bc..0x4582cf)",
        "fields": (("+0x00", "move[0]", "f64 = fild(L0)", "0x4582cf"),
                   ("+0x08", "move[1]", "f64 = fild(L1)", "0x4582d9"),
                   ("+0x10", "move[2]", "f64 = fild(L2)", "0x4582e3"),
                   ("+0x18", "extra[0]", "8 сырых байт A0", "0x4582ed"),
                   ("+0x20", "extra[1]", "8 сырых байт A1", "0x4582f7"),
                   ("+0x28", "extra[2]", "8 сырых байт A2", "0x45830b")),
    },
    41: {
        "name": "SET_HANDLESECTOR_BRIGHTNESS",
        "sig": "EL",
        "handler": 0x4583A6,
        "slot": "handle",
        "target": "world_sector",
        "store": ("0x4583f1",),
        "slot_math": ("0x4583d1", "0x4583d8"),
        "bound": "0x42d8a5 на каждый сектор (0 <= i < [0x5670dc])",
        "oob": "0x4583f6..0x458403: 'unable to obtain sector %ld'",
        "fields": (("+0x16", "light", "u8 = L & 0xff", "0x4583f1"),),
    },
    42: {
        "name": "SET_HANDLESECTOR_TYPE",
        "sig": "EL",
        "handler": 0x458429,
        "slot": "handle",
        "target": "world_sector",
        "store": ("0x42d9ff",),
        "slot_math": ("0x458457",),
        "bound": "0x42d9d5: 0 <= i < [0x5670dc]",
        "oob": "0x42d9d5 -> -1 (без сообщения)",
        "fields": (("+0x14", "kind", "u16 = value*6", "0x42d9ff"),),
    },
    43: {
        "name": "SET_HANDLESECTOR_DAMAGE",
        "sig": "EL",
        "handler": 0x458486,
        "slot": "handle",
        "target": "sector_stuff",
        "store": ("0x4584c0", "0x4584cc"),
        "slot_math": ("0x4584a7", "0x4584ae", "0x4584b1"),
        "bound": "нет",
        "oob": "проверки нет (0x4584a7..0x4584cc)",
        "fields": (("+0x40", "damage_a", "u32 = v & 0xffff", "0x4584c0"),
                   ("+0x44", "damage_b", "i32 = v >> 16 (арифм.)", "0x4584cc")),
    },
    44: {
        "name": "SET_HANDLESECTOR_FLAGS",
        "sig": "EL",
        "handler": 0x4584E7,
        "slot": "handle",
        "target": "sector_stuff",
        "store": ("0x458523",),
        "slot_math": ("0x458513", "0x45851a", "0x45851d"),
        "bound": "нет",
        "oob": "проверки нет (0x458513..0x458523)",
        "fields": (("+0x50", "flags", "u32", "0x458523"),),
    },
    45: {
        "name": "SET_HANDLESECTOR_SOUND",
        "sig": "ENVD",
        "handler": 0x45833A,
        "slot": "handle",
        "target": "sector_stuff",
        "store": ("0x46b760", "0x46b76a", "0x46b774", "0x46b77a"),
        "slot_math": ("0x458376",),
        "bound": "0x46b740: 0 <= i < 0x100 (иначе -1 + сообщение)",
        "oob": "0x46b791: 'Sector sound sector number (%ld) greater than max'",
        "note": "вызов 0x46b740(sector, N, V, D.lo, D.hi) — тот же порядок, "
                "что у op 12; push: D.hi, D.lo, V, N, sector "
                "(0x458371..0x45837f)",
        "fields": (("+0x48", "sound", "= N (a2), индекс сэмпла", "0x46b76a"),
                   ("+0x4c", "sound_count", "= V (a3), громкость", "0x46b760"),
                   ("+0x30", "sound_param", "f64 из (D.lo, D.hi) (a4/a5)", "0x46b774")),
    },
}

SOURCE_OPS = (27, 28, 29, 30, 31)
HANDLE_OPS = (40, 41, 42, 43, 44, 45)
TARGET_OPS = SOURCE_OPS + HANDLE_OPS

#: опкоды, которые пишут тот же массив с литеральным индексом — только для
#: контекста в ``--summary`` (не входят в счётчики задания)
LITERAL_OPS = {8: "SET_SECTOR_MOVEMENT",
               9: "SET_SECTOR_BRIGHTNESS",
               11: "SET_SECTOR_DAMAGE",
               12: "SET_SECTOR_SOUND",
               13: "SET_SECTOR_EVENTS",
               14: "SET_SECTOR_FLAGS"}

GENERATE_OPS = (2, 3, 4)
SET_EVENTS_OP = 13


# ---------------------------------------------------------------------------
# мелочи
# ---------------------------------------------------------------------------

def s32(value):
    return value - 0x100000000 if value >= 0x80000000 else value


def s8(value):
    return value - 0x100 if value >= 0x80 else value


def slot_of(index):
    """Байт слота ``SECTOR_STUFF``: ``index*11*8`` (``0x457fdd..0x457fec``)."""
    return index * SECTOR_STUFF_STRIDE


def slot_formula(index):
    """Та же формула по шагам, как её считает код: 11*index, затем *8."""
    five = index * 5           # lea ecx,[edi+edi*4]
    eleven = index + five * 2  # lea edi,[edi+ecx*2]
    return eleven * 8          # shl edi,0x3


def world_sector_offset(index):
    """Байт записи сектора мира ``0x5670d8``: ``index*0x18``."""
    return index * 0x18


def resolve_game(path):
    if path:
        return path
    if os.path.isdir(MIRROR):
        return MIRROR
    return DEFAULT_GAME


def as_double(raw):
    """``A``/``D``-операнд: ``gm.sbtvm`` уже отдаёт его как ``double``."""
    if isinstance(raw, float):
        return raw
    return struct.unpack("<d", (raw & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little"))[0]


def fmt_double(raw64):
    value = as_double(raw64)
    if value != value or value in (float("inf"), float("-inf")):
        return repr(value)
    if value == int(value) and abs(value) < 1e15:
        return "%d.0" % int(value)
    return "%.10g" % value


def script_rows(script):
    """``[(flags, id, offset)]`` — три колонки таблицы сущностей."""
    n = script.n
    if n <= 0 or len(script.handles) != 12 * n:
        return []
    cols = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
    return [(cols[i], s32(cols[n + i]), cols[2 * n + i]) for i in range(n)]


def walk(insns):
    """Все инструкции, включая тела ``IF``/``ELSE``."""
    for i in insns:
        yield i
        if isinstance(i.body, list):
            yield from walk(i.body)


def block_ranges(insns):
    """``[(start, end)]`` подскриптов верхнего уровня (конец — ``0xFF``)."""
    out, start = [], None
    for i in insns:
        if start is None:
            start = i.off
        if i.name == "END":
            out.append((start, i.off + 1))
            start = None
    return out


def block_instructions(insns):
    """``{start: [insn, ...]}`` по подскриптам верхнего уровня."""
    out, cur = {}, None
    for i in insns:
        if cur is None:
            cur = []
        cur.append(i)
        if i.name == "END":
            out[cur[0].off] = cur
            cur = None
    return out


def block_of(offset, starts):
    """Начало подскрипта, содержащего *offset*."""
    if not starts:
        return offset
    best = starts[0]
    for start in starts:
        if start <= offset:
            best = start
        else:
            break
    return best


# ---------------------------------------------------------------------------
# операнды по опкоду
# ---------------------------------------------------------------------------

def operands_of(op, args):
    """Человекочитаемая расшифровка операндов (порядок — как в байткоде)."""
    if op in (27, 40):
        base = 1 if op == 40 else 0          # у 40 первый операнд — хэндл
        return {
            "handle": (args[0] if op == 40 else None),
            "move": [s32(args[base + k]) for k in range(3)],
            "extra": [fmt_double(args[base + 3 + k]) for k in range(3)],
        }
    if op in (28, 29, 41, 44):
        value = args[-1] & 0xFFFFFFFF
        return {"handle": (args[0] if op in (41, 44) else None),
                "value": value,
                "flags": (value if op in (29, 44) else None),
                "brightness": (s8(value & 0xFF) if op in (28, 41) else None)}
    if op in (30, 42):
        value = s32(args[-1])
        return {"handle": (args[0] if op == 42 else None),
                "value": value, "kind": (value * 6) & 0xFFFF}
    if op in (31, 43):
        value = args[-1] & 0xFFFFFFFF
        return {"handle": (args[0] if op == 43 else None),
                "value": value,
                "damage_a": value & 0xFFFF,
                "damage_b": s32(value) >> 16}
    if op == 45:
        return {"handle": args[0], "n": s32(args[1]), "v": s32(args[2]),
                "param": fmt_double(args[3])}
    return {"raw": [int(a) for a in args]}


def value_of(op, args):
    """Одно число, по которому строится гистограмма (``None`` — нет такого)."""
    info = operands_of(op, args)
    for key in ("brightness", "flags", "kind", "damage_a", "v"):
        if key in info and info[key] is not None:
            return int(info[key])
    return None


# ---------------------------------------------------------------------------
# граф запусков: откуда берётся «сектор источника» у 27-31
# ---------------------------------------------------------------------------

def source_origins(block_map, rows, init_start):
    """Возможные секторы источника для каждого подскрипта.

    Модель (все переходы доказаны кодом):

    * главный скрипт ``offset 0`` запускается с ``arg3 = 0`` — **гипотеза**:
      ``0x45fa00`` передаёт свой ``arg2`` в ``0x457390`` (``0x45faba..0x45fac2``),
      а инициализационные вызовы ``0x45fa00`` (``0x442643``, ``0x443253`` …)
      кладут в ``arg2`` регистр ``ebx`` = «пустое» значение (он же служит
      нулём в проверках ``cmp [0x5a46d4],ebx``); константы ``push 0`` для
      сектора в коде нет;
    * ``GENERATE`` (2/3/4) передаёт *свой* ``edi`` в ``0x479960``
      (``0x4575bb push edi``) -> источник наследуется;
    * ``SET_SECTOR_EVENTS <sector> <e1> <e2>`` (опкод 13) кладёт id событий в
      слот ``sector`` (``0x4579d6``/``0x4579e0``); когда игрок входит в этот
      сектор, событие постится с ``sector`` (``0x445095`` -> ``push eax`` =
      ``[0x81c27c]``) и скрипт строки с таким id стартует с этим сектором
      (``0x479a3a``: 16-битное поле записи события -> ``arg2 0x479960``).

    Возвращает ``{block: {"sectors": [..], "complete": bool, "why": set()}}``.
    """
    rows_by_id = {}
    for flags, eid, off in rows:
        rows_by_id.setdefault(eid, []).append(off)

    origins = {start: set() for start in block_map}
    complete = {start: True for start in block_map}
    why = {start: set() for start in block_map}

    # 1. главный скрипт
    if init_start in origins:
        origins[init_start].add(0)
        why[init_start].add("init")

    # 2. события секторов: литеральный слот + два id события
    for start, insns in block_map.items():
        for insn in insns:
            if insn.op != SET_EVENTS_OP:
                continue
            sector = s32(insn.args[0])
            for event_id in (s32(insn.args[1]), s32(insn.args[2])):
                for off in rows_by_id.get(event_id, []):
                    if off in origins and 0 <= sector < SECTOR_STUFF_SLOTS:
                        origins[off].add(sector)
                        why[off].add("sector_event:%d@%d" % (sector, start))

    # 3. всё, что вообще называется в коде как GENERATE/событие — «объяснено»;
    #    остальные строки может дёрнуть только движок -> маршрут неизвестен
    named = set()
    for insns in block_map.values():
        for insn in insns:
            if insn.op in GENERATE_OPS:
                named.add(s32(insn.args[0]))
            elif insn.op == SET_EVENTS_OP:
                named.add(s32(insn.args[1]))
                named.add(s32(insn.args[2]))
    for flags, eid, off in rows:
        if eid not in named and off in complete:
            complete[off] = False
            why[off].add("engine:%d" % eid)

    # 4. неподвижная точка по GENERATE
    changed = True
    while changed:
        changed = False
        for start, insns in block_map.items():
            for insn in insns:
                if insn.op not in GENERATE_OPS:
                    continue
                for off in rows_by_id.get(s32(insn.args[0]), []):
                    if off not in origins:
                        continue
                    before = (len(origins[off]), complete[off])
                    origins[off] |= origins[start]
                    if not complete[start]:
                        complete[off] = False
                    if before != (len(origins[off]), complete[off]):
                        changed = True

    return {start: {"sectors": sorted(origins[start]),
                    "complete": complete[start],
                    "why": sorted(why[start])}
            for start in block_map}


# ---------------------------------------------------------------------------
# один уровень
# ---------------------------------------------------------------------------

def analyze_level(stem, sbt_path, sbw_path=None):
    tree = sbtmod.parse_file(sbt_path)
    script = tree.records_of(SCRIPT_RECORD)[0].payload
    insns, end = sbtvm.decode(script.code)
    if end != len(script.code):
        raise ValueError("%s: decode stopped at 0x%x of 0x%x"
                         % (stem, end, len(script.code)))

    rows = script_rows(script)
    blocks = block_ranges(insns)
    starts = [b[0] for b in blocks]
    block_map = block_instructions(insns)
    init_start = starts[0] if starts else 0

    handle_rows = []
    records = tree.records_of(SECTOR_HANDLE_RECORD)
    if records:
        table = records[0].payload
        handle_rows = [(int(t), int(s)) for t, s in zip(table.tags, table.sectors)]

    world_sectors = None
    if sbw_path and os.path.isfile(sbw_path):
        try:
            world_sectors = read_sbw_sector_count(sbw_path)
        except Exception:                                        # noqa: BLE001
            world_sectors = None

    origins = source_origins(block_map, rows, init_start)
    rows_at = {}
    for flags, eid, off in rows:
        rows_at.setdefault(off, []).append((flags, eid))

    commands = []
    for insn in walk(insns):
        if insn.op not in COMMANDS:
            continue
        spec = COMMANDS[insn.op]
        start = block_of(insn.off, starts)
        origin = origins.get(start, {"sectors": [], "complete": False, "why": []})
        entry = {
            "op": insn.op,
            "name": spec["name"],
            "handler": "0x%06x" % spec["handler"],
            "offset": insn.off,
            "block": start,
            "target": spec["target"],
            "slot_kind": spec["slot"],
            "operands": operands_of(insn.op, insn.args),
            "fields": [{"off": f[0], "field": f[1], "how": f[2], "store": f[3]}
                       for f in spec["fields"]],
            "row_ids": sorted({eid for _f, eid in rows_at.get(start, [])}),
            "row_flags": sorted({f for f, _e in rows_at.get(start, [])}),
        }
        if spec["slot"] == "source":
            entry["slot"] = {
                "kind": "source",
                "index": None,
                "sectors": list(origin["sectors"]),
                "complete": bool(origin["complete"]),
                "why": list(origin["why"]),
                "byte": None,
            }
        else:
            tag = s32(insn.args[0])
            sectors = sorted({sec for tg, sec in handle_rows if tg == tag})
            entry["slot"] = {
                "kind": "handle",
                "tag": tag,
                "index": (sectors[0] if len(sectors) == 1 else None),
                "sectors": sectors,
                "complete": bool(sectors),
                "why": ["SECTOR_HANDLE:%d" % tag] if sectors else [],
                "byte": (slot_of(sectors[0]) if len(sectors) == 1 else None),
                "rows": sum(1 for tg, _s in handle_rows if tg == tag),
            }
            if sectors and world_sectors is not None:
                entry["slot"]["out_of_world"] = [
                    s for s in sectors if not (0 <= s < world_sectors)]
        entry["value"] = value_of(insn.op, insn.args)
        if entry["slot"]["kind"] == "source" and len(entry["slot"]["sectors"]) == 1:
            index = entry["slot"]["sectors"][0]
            entry["slot"]["index"] = index
            entry["slot"]["byte"] = (slot_of(index)
                                     if spec["target"] == "sector_stuff"
                                     else world_sector_offset(index))
        commands.append(entry)

    stuff_slots = sorted({s for c in commands if c["target"] == "sector_stuff"
                          for s in c["slot"]["sectors"]})
    world_slots = sorted({s for c in commands if c["target"] == "world_sector"
                          for s in c["slot"]["sectors"]})
    counts = {str(op): sum(1 for c in commands if c["op"] == op) for op in TARGET_OPS}
    literal_counts = {str(op): 0 for op in LITERAL_OPS}
    literal_stuff = set()
    literal_world = set()
    for insn in walk(insns):
        if insn.op not in LITERAL_OPS:
            continue
        literal_counts[str(insn.op)] += 1
        index = s32(insn.args[0])
        if not 0 <= index < SECTOR_STUFF_SLOTS:
            continue
        if insn.op == 9:                     # SET_SECTOR_BRIGHTNESS
            if world_sectors is None or index < world_sectors:
                literal_world.add(index)
        elif insn.op == 10:                  # SET_SECTOR_TYPE
            if world_sectors is None or index < world_sectors:
                literal_world.add(index)
        else:
            literal_stuff.add(index)

    return {
        "level": stem,
        "sbt": sbt_path,
        "sbw": sbw_path if world_sectors is not None else None,
        "script": {"code_bytes": len(script.code), "rows": len(rows),
                   "blocks": len(blocks), "init_block": init_start},
        "world": {"sectors": world_sectors,
                  "sector_stuff_record": bool(tree.records_of(SECTOR_STUFF_RECORD)),
                  "sector_handle_record": bool(records),
                  "sector_handles": len(handle_rows)},
        "counts": counts,
        "literal_counts": literal_counts,
        "literal_slots": {"sector_stuff": sorted(literal_stuff),
                          "world_sector": sorted(literal_world)},
        "commands": commands,
        "slots": {
            "sector_stuff": stuff_slots,
            "world_sector": world_slots,
            "sector_stuff_bytes": [slot_of(s) for s in stuff_slots],
            "world_sector_bytes": [world_sector_offset(s) for s in world_slots],
        },
    }


def read_sbw_sector_count(path):
    """Число секторов ``.sbw`` — через оракул ``gm.sbw`` (шапка переменной длины)."""
    try:
        from gm import sbw as sbwmod      # noqa: PLC0415
    except Exception:                     # noqa: BLE001
        return None
    return len(sbwmod.parse_file(path).sectors)


# ---------------------------------------------------------------------------
# агрегат по всем уровням
# ---------------------------------------------------------------------------

def aggregate(levels):
    counts = {str(op): 0 for op in TARGET_OPS}
    literal_counts = {str(op): 0 for op in LITERAL_OPS}
    values = {str(op): {} for op in TARGET_OPS}
    slot_stats = {
        "source_commands": 0,
        "source_with_known_sector": 0,
        "source_ambiguous": 0,
        "source_unknown": 0,
        "handle_commands": 0,
        "handle_resolved": 0,
        "handle_unresolved": 0,
        "handle_rows_total": 0,
        "out_of_world": 0,
    }
    stuff_slots = set()
    world_slots = set()
    for level in levels:
        for op in TARGET_OPS:
            counts[str(op)] += level["counts"][str(op)]
        for op in LITERAL_OPS:
            literal_counts[str(op)] += level["literal_counts"][str(op)]
        for cmd in level["commands"]:
            op = str(cmd["op"])
            value = cmd.get("value")
            if value is not None:
                values[op][str(value)] = values[op].get(str(value), 0) + 1
            slot = cmd["slot"]
            if slot["kind"] == "source":
                slot_stats["source_commands"] += 1
                if len(slot["sectors"]) == 1:
                    slot_stats["source_with_known_sector"] += 1
                elif len(slot["sectors"]) > 1:
                    slot_stats["source_ambiguous"] += 1
                else:
                    slot_stats["source_unknown"] += 1
            else:
                slot_stats["handle_commands"] += 1
                slot_stats["handle_rows_total"] += slot.get("rows", 0)
                if slot["complete"]:
                    slot_stats["handle_resolved"] += 1
                else:
                    slot_stats["handle_unresolved"] += 1
                slot_stats["out_of_world"] += len(slot.get("out_of_world") or [])
        stuff_slots.update(level["slots"]["sector_stuff"])
        world_slots.update(level["slots"]["world_sector"])
    return {
        "counts": counts,
        "literal_counts": literal_counts,
        "values": values,
        "slots": {
            "sector_stuff_union": sorted(stuff_slots),
            "world_sector_union": sorted(world_slots),
        },
        "stats": slot_stats,
        "levels": len(levels),
    }


# ---------------------------------------------------------------------------
# вывод
# ---------------------------------------------------------------------------

def print_map():
    print("опкод  команда                          sig      обработчик  слот"
          "          цель           запись")
    for op in TARGET_OPS:
        spec = COMMANDS[op]
        print("%5d  %-32s %-8s 0x%06x   %-13s %-14s %s"
              % (op, spec["name"], spec["sig"], spec["handler"],
                 spec["slot"], spec["target"], spec["store"][0]))
    print()
    print("слот SECTOR_STUFF  = index*11*8 = index*0x58  (0x457fdd/0x457fe9/"
          "0x457fec, 0x4582c6/0x4582c9/0x4582cc)")
    print("сектор мира 0x5670d8 = index*0x18 (0x42d8c0 imul ecx,ecx,0x18)")
    print("источник = arg3 0x457390 (0x4573ab mov edi,[ebp+0x10]); "
          "хэндл = строка SECTOR_HANDLE (0x463900 -> 0x5b1df8/0x617fb8)")
    print()
    print("поля: опкод -> что и куда пишется")
    for op in TARGET_OPS:
        spec = COMMANDS[op]
        for off, field, how, store in spec["fields"]:
            target = ("SECTOR_STUFF" if spec["target"] == "sector_stuff"
                      else "world sector 0x5670d8")
            print("  %2d %-30s %-7s %-30s %s %s"
                  % (op, spec["name"], off, field, target, store))
        print("     границы: %s" % spec["bound"])
        print("     вне диапазона: %s" % spec["oob"])


def _fmt_list(values, limit=12):
    values = list(values)
    if len(values) <= limit:
        return ",".join(str(v) for v in values)
    return "%s,...(+%d)" % (",".join(str(v) for v in values[:limit]),
                            len(values) - limit)


def print_level(level):
    sc = level["script"]
    print("== %s ==" % level["level"])
    print("  .sbt %s" % level["sbt"])
    print("  код %d Б, строк %d, подскриптов %d, init-блок 0x%x"
          % (sc["code_bytes"], sc["rows"], sc["blocks"], sc["init_block"]))
    world = level["world"]
    print("  .sbw секторов %s; SECTOR_STUFF(8) %s; SECTOR_HANDLE(17) %s (%d строк)"
          % (world["sectors"], "есть" if world["sector_stuff_record"] else "нет",
             "есть" if world["sector_handle_record"] else "нет",
             world["sector_handles"]))
    print("  команды: " + "  ".join("%d:%d" % (op, level["counts"][str(op)])
                                    for op in TARGET_OPS))
    print("  литеральные (контекст): "
          + "  ".join("%d:%d" % (op, level["literal_counts"][str(op)])
                      for op in LITERAL_OPS))
    print("  слоты SECTOR_STUFF: %s" % (_fmt_list(level["slots"]["sector_stuff"])
                                         or "нет"))
    print("  слоты world sector: %s" % (_fmt_list(level["slots"]["world_sector"])
                                         or "нет"))
    for cmd in level["commands"]:
        slot = cmd["slot"]
        if slot["kind"] == "source":
            target = ("источник: 0%s" % _fmt_list(slot["sectors"]) if slot["sectors"]
                      else "источник: ?")
            if len(slot["sectors"]) > 1:
                target += " (неоднозначно)"
            if not slot["complete"]:
                target += " [маршрут не объяснён]"
        else:
            target = ("хэндл %d -> сектор %s" % (slot["tag"],
                                                 _fmt_list(slot["sectors"]))
                      if slot["sectors"] else
                      "хэндл %d -> нет строки в SECTOR_HANDLE" % slot["tag"])
        ops = cmd["operands"]
        detail = ", ".join("%s=%s" % (k, v) for k, v in ops.items()
                           if v is not None)
        print("  +0x%05x %-30s %-14s %s" % (cmd["offset"], cmd["name"],
                                            cmd["handler"], detail))
        print("           слот: %s%s" % (
            target,
            ("" if slot.get("byte") is None
             else "  (байт 0x%x)" % slot["byte"])))
        print("           блок 0x%x, строка id %s, flags %s"
              % (cmd["block"], _fmt_list(cmd["row_ids"], 6),
                 _fmt_list(cmd["row_flags"], 6)))
        print("           поля: " + ", ".join("%s %s" % (f["off"], f["field"])
                                              for f in cmd["fields"]))


def print_summary(result):
    levels = result["levels"]
    summary = result["summary"]
    header = ("уровень   " + " ".join("%2d" % op for op in SOURCE_OPS) + " |"
              + "".join(" %2d" % op for op in HANDLE_OPS)
              + " | SS  WS  SRC  init")
    print(header)
    print("-" * len(header))
    for level in levels:
        c = level["counts"]
        source = sum(c[str(op)] for op in SOURCE_OPS)
        init = sum(1 for cmd in level["commands"]
                   if cmd["slot"]["kind"] == "source"
                   and cmd["slot"].get("index") == 0
                   and "init" in (cmd["slot"].get("why") or []))
        print("%-9s " % level["level"]
              + " ".join("%2d" % c[str(op)] for op in SOURCE_OPS) + " |"
              + "".join(" %2d" % c[str(op)] for op in HANDLE_OPS)
              + " | %2d  %2d  %3d  %3d"
              % (len(level["slots"]["sector_stuff"]),
                 len(level["slots"]["world_sector"]), source, init))
    counts = summary["counts"]
    stats = summary["stats"]
    print("-" * len(header))
    print("%-9s " % "ИТОГО"
          + " ".join("%2d" % counts[str(op)] for op in SOURCE_OPS) + " |"
          + "".join(" %2d" % counts[str(op)] for op in HANDLE_OPS)
          + " |")
    print()
    print("SS/WS — сколько разных слотов статически адресуется на уровне")
    print("         (SECTOR_STUFF / запись сектора мира); SRC — команд 27-31;")
    print("         init — из них в главном скрипте (источник = 0 доказанно).")
    print()
    print("опкод  команда                          всего  слот")
    for op in TARGET_OPS:
        spec = COMMANDS[op]
        if spec["slot"] == "source":
            note = "сектор источника (arg3 0x457390)"
        else:
            note = "сектор строки SECTOR_HANDLE"
        print("%5d  %-32s %5d  %s" % (op, spec["name"], counts[str(op)], note))
    print()
    print("источник: известен %d, неоднозначен %d, неизвестен %d (из %d)"
          % (stats["source_with_known_sector"], stats["source_ambiguous"],
             stats["source_unknown"], stats["source_commands"]))
    print("хэндлы:   разрешено %d, без строки %d (из %d); совпадений "
          "тег-строка %d"
          % (stats["handle_resolved"], stats["handle_unresolved"],
             stats["handle_commands"], stats["handle_rows_total"]))
    print("          секторов вне диапазона .sbw: %d" % stats["out_of_world"])
    print()
    print("литеральные команды того же массива (контекст): "
          + ", ".join("%d:%d" % (op, summary["literal_counts"][str(op)])
                      for op in LITERAL_OPS))
    print()
    print("значения по опкодам (гистограмма):")
    for op in TARGET_OPS:
        hist = summary["values"][str(op)]
        if not hist:
            continue
        items = sorted(((int(k), v) for k, v in hist.items()))
        print("  %2d %-30s %s" % (op, COMMANDS[op]["name"],
                                  ", ".join("%d x%d" % kv for kv in items[:14])
                                  + ("" if len(items) <= 14 else ", ...")))


def analyze(game, level=None):
    if level:
        stem = os.path.basename(level)
        if stem.lower().endswith(".sbt"):
            stem = stem[:-4]
        sbt_path = level if os.path.isfile(level) else os.path.join(game, stem + ".sbt")
        if not os.path.isfile(sbt_path):
            raise SystemExit("level not found: %s" % level)
        sbw_path = os.path.join(game, stem + ".sbw")
        levels = [analyze_level(stem, sbt_path,
                                sbw_path if os.path.isfile(sbw_path) else None)]
    else:
        levels = []
        for path in sorted(glob.glob(os.path.join(game, "*.sbt"))):
            stem = os.path.basename(path)[:-4]
            sbw_path = os.path.join(game, stem + ".sbw")
            levels.append(analyze_level(stem, path,
                                        sbw_path if os.path.isfile(sbw_path) else None))
    result = {"game": game, "levels": levels, "summary": aggregate(levels)}
    result["code"] = {
        "sector_stuff": "0x800b60, 256 x 0x58, slot = index*11*8 (0x457fdd..)",
        "world_sector": "0x5670d8, stride 0x18 (0x42d8c0), light +0x16, kind +0x14",
        "source_arg": "arg3 of 0x457390 (0x4573ab mov edi,[ebp+0x10])",
        "source_saved": "0x616258[i] (0x45c4a5 on SLEEP), ticker 0x45734b",
        "source_generate": "0x4575bb push edi -> 0x479960 -> 0x45fa00 -> 0x457390",
        "handle_table": "SECTOR_HANDLE (17): 0x5e3de0 count, 0x5b1df8 tags, "
                        "0x617fb8 sectors, loader 0x463900",
        "commands": {str(op): {"name": spec["name"], "handler": "0x%06x" % spec["handler"],
                               "slot": spec["slot"], "target": spec["target"],
                               "store": list(spec["store"]), "bound": spec["bound"],
                               "oob": spec["oob"]}
                     for op, spec in COMMANDS.items()},
    }
    return result


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _synth_sbt(code, rows, handles):
    """``.sbt``: одна таблица ``SECTOR_HANDLE`` (тип 17) и один ``SCRIPT``."""
    out = bytearray(sbtmod.MAGIC + b"\0" * (80 - len(sbtmod.MAGIC))
                    + struct.pack("<I", 0))
    tags = [tag for tag, _sec in handles]
    secs = [sec for _tag, sec in handles]
    payload = struct.pack("<I", len(tags))
    payload += struct.pack("<%dI" % len(tags), *tags)
    payload += struct.pack("<%dI" % len(secs), *secs)
    out += bytes((SECTOR_HANDLE_RECORD,)) + payload
    script = sbtmod.Script(
        count=0, n=len(rows), m=len(code), flag=0, things=b"",
        handles=(b"".join(struct.pack("<I", r[0] & 0xFFFFFFFF) for r in rows)
                 + b"".join(struct.pack("<I", r[1] & 0xFFFFFFFF) for r in rows)
                 + b"".join(struct.pack("<I", r[2] & 0xFFFFFFFF) for r in rows)),
        code=code, tables=b"")
    out += bytes((SCRIPT_RECORD,)) + script.encode()
    out += bytes((255,))
    return bytes(out)


def _synth_code():
    """init-блок: 28/8/13(слот 11 -> события 7); блок строки id 7: 41/43/44/40/45/28."""
    code = bytearray()
    code += bytes((28,)) + struct.pack("<i", 32)                 # источник=0 (init)
    # SET_SECTOR_MOVEMENT = LLLLAAA: индекс + 3 int + 3*8 байт
    code += (bytes((8,)) + struct.pack("<4i", 3, 1, 2, 3) + b"\0" * 24)
    code += bytes((13,)) + struct.pack("<3i", 11, 7, 0)          # события слота 11
    code += bytes((0xFF,))
    event_off = len(code)
    code += bytes((41,)) + struct.pack("<iI", 5, 0xFFFFFF80)     # хэндл 5 -> 7,9
    code += bytes((43,)) + struct.pack("<ii", 5, 0x00040005)     # damage
    code += bytes((44,)) + struct.pack("<iI", 6, 0x10)           # флаги
    code += bytes((40,)) + struct.pack("<i", 6) + struct.pack(
        "<3i", 10, 0, 0) + b"\0" * 24                            # movement
    code += bytes((45,)) + struct.pack("<i", 6) + struct.pack(
        "<2i", 7, 8) + b"\0" * 8                                 # sound
    code += bytes((28,)) + struct.pack("<i", 48)                 # слот 11 (событие)
    code += bytes((0xFF,))
    rows = [(0x1FF, 7, event_off)]
    return bytes(code), rows, event_off


def self_test():
    ok = 0
    bad = 0

    def check(name, cond, detail=""):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("ok    %s%s" % (name, (" — " + detail) if detail else ""))
        else:
            bad += 1
            print("FAIL  %s%s" % (name, (" — " + detail) if detail else ""))

    def skip(name, why):
        print("skip  %s — %s" % (name, why))

    # --- 1. арифметика слота ------------------------------------------------
    check("slot = index*11*8 = index*0x58 for 0..255",
          all(slot_of(i) == slot_formula(i) == i * 0x58 for i in range(256))
          and slot_of(255) == 255 * 0x58)
    check("256 slots = 0x5800 bytes",
          SECTOR_STUFF_SLOTS * SECTOR_STUFF_STRIDE == 0x5800)
    check("world sector offset = index*0x18", world_sector_offset(255) == 0x17E8)

    # --- 2. таблица опкодов против оракула ----------------------------------
    table = dict(getattr(sbtvm, "COMMANDS", {}) or {})
    bad_sig = []
    for op in TARGET_OPS:
        name, sig = table.get(op, (None, None))
        if name != COMMANDS[op]["name"] or sig != COMMANDS[op]["sig"]:
            bad_sig.append("%d:%s/%s" % (op, name, sig))
    check("gm.sbtvm: имена и сигнатуры 27-31/40-45 совпадают",
          not bad_sig, ", ".join(bad_sig) or "8/8")
    check("набор опкодов = {27..31} | {40..45}",
          set(COMMANDS) == set(SOURCE_OPS) | set(HANDLE_OPS)
          and sorted(COMMANDS) == list(TARGET_OPS))

    # --- 3. дизассемблер (нужен exe/листинг; иначе skip) --------------------
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import exelib                                     # noqa: PLC0415
        have_exe = bool(exelib.EXE) and os.path.isfile(exelib.EXE)
        if have_exe:
            exelib.disasm(0x4573AB, 0x4573AC)             # листинг/индекс готовы?
    except Exception as exc:                              # noqa: BLE001
        have_exe = False
        exelib = None
        skip("exe-проверки (диспетчер, байты обработчиков)", str(exc))

    if have_exe:
        off = exelib.va2off(0x45C500)
        raw = exelib.data()[off:off + 256 * 4]
        table_ok, wrong = True, []
        for op in TARGET_OPS:
            entry = struct.unpack_from("<I", raw, (op - 1) * 4)[0]
            if entry != COMMANDS[op]["handler"]:
                table_ok = False
                wrong.append("%d:0x%x!=0x%x" % (op, entry, COMMANDS[op]["handler"]))
        check("диспетчер 0x45c500[op-1] = обработчик", table_ok,
              ", ".join(wrong) or "10/10")

        def text(lo, hi):
            return " ".join(exelib.disasm(lo, hi))

        body27 = text(0x457FDD, 0x457FF0)
        check("27: slot = ((i + 5i*2) << 3)",
              "lea    ecx,[edi+edi*4]" in body27
              and "lea    edi,[edi+ecx*2]" in body27
              and "shl    edi,0x3" in body27, "0x457fdd/0x457fe9/0x457fec")
        body40 = text(0x4582BC, 0x4582D0)
        check("40: sector = [0x617fb8 + i*4], slot = 11*sector << 3",
              "0x617fb8" in body40 and "lea    edi,[eax+eax*4]" in body40
              and "lea    eax,[eax+edi*2]" in body40
              and "shl    eax,0x3" in body40, "0x4582bc..0x4582cc")
        check("27/29/31: границы 0..0xff по edi",
              "test   edi,edi" in text(0x457FC6, 0x457FD6)
              and "test   edi,edi" in text(0x458111, 0x458120)
              and "test   edi,edi" in text(0x4580CF, 0x4580E2))
        check("40/43/44: проверки границ нет",
              "test   edi,edi" not in text(0x45829B, 0x4582C0)
              and "cmp    edi,0x100" not in text(0x45829B, 0x4582C0)
              and "0x100" not in text(0x458486, 0x4584B0)
              and "0x100" not in text(0x4584E7, 0x458515))
        check("41: store [eax+0x16] через геттер 0x42d8a5",
              "0x42d8a5" in text(0x4583A6, 0x4583E0)
              and "mov    BYTE PTR [eax+0x16],cl" in text(0x4583E5, 0x4583F4))
        body45 = text(0x458365, 0x458385)
        check("45: sector = [edi*4+0x617fb8]; push D.hi, D.lo, V, N, sector "
              "(a2 = N, a3 = V, a4 = D.lo, a5 = D.hi — как у op 12)",
              "mov    edx,DWORD PTR [edi*4+0x617fb8]"
              in text(0x458376, 0x45837A)
              and "mov    ecx,DWORD PTR [ebx+esi*1]" in body45      # N
              and "mov    eax,DWORD PTR [ebx+esi*1+0x4]" in body45  # V
              and "mov    edx,DWORD PTR [ebx+esi*1+0x8]" in body45  # D.lo
              and "mov    ecx,DWORD PTR [ebx+esi*1+0xc]" in body45  # D.hi
              and body45.count("push   ") == 5)
        check("источник: edi = arg3 0x457390",
              "mov    edi,DWORD PTR [ebp+0x10]" in text(0x457390, 0x4573B0))
        check("SLEEP сохраняет источник в 0x616258",
              "mov    DWORD PTR [ecx*4+0x616258],edi"
              in text(0x45C4A5, 0x45C4AC))
        check("GENERATE передаёт edi дальше",
              "push   edi" in text(0x4575B5, 0x4575BD))
        check("SECTOR_HANDLE-загрузчик 0x463900 читает 0x5b1df8/0x617fb8",
              "0x5b1df8" in text(0x463900, 0x463985)
              and "0x617fb8" in text(0x463900, 0x463985))
    else:
        print("skip  exe-проверки (диспетчер, байты обработчиков) — нет exe")

    # --- 4. синтетика: источник/хэндл/литерал ------------------------------
    import tempfile
    code, rows, event_off = _synth_code()
    data = _synth_sbt(code, rows, [(5, 7), (5, 9), (6, 4)])
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "synth.sbt")
        with open(path, "wb") as fh:
            fh.write(data)
        lv = analyze_level("synth", path, None)
    by_off = {c["offset"]: c for c in lv["commands"]}
    n = len(lv["commands"])
    init_28 = [c for c in lv["commands"] if c["op"] == 28 and c["offset"] == 0]
    check("синтетика: 7 команд 27-31/40-45", n == 7, "найдено %d" % n)
    check("синтетика: 28 в init -> слот 0 (доказан)",
          init_28 and init_28[0]["slot"]["sectors"] == [0]
          and init_28[0]["slot"]["byte"] == 0,
          str(init_28[0]["slot"]) if init_28 else "нет")
    h41 = [c for c in lv["commands"] if c["op"] == 41][0]
    check("синтетика: 41 хэндл 5 -> секторы [7,9]",
          h41["slot"]["sectors"] == [7, 9] and h41["slot"]["tag"] == 5,
          str(h41["slot"]["sectors"]))
    h43 = [c for c in lv["commands"] if c["op"] == 43][0]
    check("синтетика: 43 damage (5,4) -> dmg_a=5, dmg_b=4",
          h43["operands"]["damage_a"] == 5 and h43["operands"]["damage_b"] == 4,
          str(h43["operands"]))
    h44 = [c for c in lv["commands"] if c["op"] == 44][0]
    check("синтетика: 44 flags 0x10 -> сектор 4, байт 4*0x58",
          h44["slot"]["sectors"] == [4] and h44["slot"]["byte"] == slot_of(4),
          str(h44["slot"]))
    h40 = [c for c in lv["commands"] if c["op"] == 40][0]
    check("синтетика: 40 move -> сектор 4 и +0x50 у 44 в том же блоке",
          h40["slot"]["sectors"] == [4] and h40["operands"]["move"] == [10, 0, 0],
          str(h40["operands"]))
    ev28 = [c for c in lv["commands"] if c["op"] == 28 and c["offset"] > 0]
    check("синтетика: 28 в блоке, запущенном из SET_SECTOR_EVENTS 11 -> 11",
          ev28 and ev28[0]["slot"]["sectors"] == [11]
          and ev28[0]["slot"]["index"] == 11,
          str(ev28[0]["slot"]) if ev28 else "нет")
    lit = lv["literal_counts"]
    check("синтетика: литеральные 8 и 13 -> слоты [3, 11]",
          lit["8"] == 1 and lit["13"] == 1
          and lv["literal_slots"]["sector_stuff"] == [3, 11],
          "slots=%s" % lv["literal_slots"]["sector_stuff"])

    # --- 5. корпус: 36 уровней ---------------------------------------------
    game = resolve_game(None)
    if not os.path.isdir(game):
        skip("корпус 36 уровней", "нет установки: %s" % game)
    else:
        result = analyze(game)
        levels = result["levels"]
        check("корпус: 36 уровней разобраны", len(levels) == 36,
              "%d" % len(levels))
        counts = result["summary"]["counts"]
        check("корпус: SET_SOURCESECTOR_BRIGHTNESS = 562 (28)",
              counts["28"] == 562, "%d" % counts["28"])
        check("корпус: SET_HANDLESECTOR_BRIGHTNESS = 332 (41)",
              counts["41"] == 332, "%d" % counts["41"])
        check("корпус: SET_SECTOR_BRIGHTNESS = 9 (9, литеральная)",
              result["summary"]["literal_counts"]["9"] == 9)
        check("корпус: записи SECTOR_STUFF (8) нет ни в одном уровне",
              all(not lv["world"]["sector_stuff_record"] for lv in levels))
        check("корпус: SECTOR_HANDLE (17) не пуст (33 уровня, 900 строк)",
              sum(1 for lv in levels if lv["world"]["sector_handle_record"]) == 33
              and sum(lv["world"]["sector_handles"] for lv in levels) == 900,
              "%d уровней, %d строк"
              % (sum(1 for lv in levels if lv["world"]["sector_handle_record"]),
                 sum(lv["world"]["sector_handles"] for lv in levels)))
        unresolved = [(lv["level"], c["op"], c["slot"]["tag"])
                      for lv in levels for c in lv["commands"]
                      if c["slot"]["kind"] == "handle" and not c["slot"]["complete"]]
        check("корпус: 1017/1023 E-операндов 40-45 находят строку таблицы "
              "(6 — нет, это no-op оригинала)",
              len(unresolved) == 6,
              ", ".join("%s:%d:%d" % u for u in unresolved))
        oow = [c for lv in levels for c in lv["commands"]
               if c["slot"].get("out_of_world")]
        check("корпус: все разрешённые секторы < числа секторов .sbw",
              not oow, "%d вне мира" % len(oow))
        check("корпус: --json детерминирован",
              json.dumps(result, sort_keys=True, ensure_ascii=False)
              == json.dumps(analyze(game), sort_keys=True, ensure_ascii=False))

    print()
    print("%d ok, %d fail" % (ok, bad))
    return 0 if bad == 0 else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="SECTOR_STUFF: какой слот адресуют команды 27-31/40-45")
    ap.add_argument("--game", help="каталог установки (по умолчанию зеркало)")
    ap.add_argument("--level", help="один уровень (stem или путь к .sbt)")
    ap.add_argument("--summary", action="store_true", help="таблица по уровням")
    ap.add_argument("--map", action="store_true", help="таблица опкодов и полей")
    ap.add_argument("--json", action="store_true", help="JSON в stdout")
    ap.add_argument("--self-test", action="store_true", help="самопроверка")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.map:
        print_map()
        return 0

    game = resolve_game(args.game)
    result = analyze(game, args.level)
    if args.json:
        json.dump(result, sys.stdout, ensure_ascii=False, sort_keys=True,
                  separators=(",", ":"))
        sys.stdout.write("\n")
        return 0
    if args.level:
        print_level(result["levels"][0])
        return 0
    print_summary(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
