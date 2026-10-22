#!/usr/bin/env python3
"""Скриптовый свет сектора: ``sector+0x16`` на данных 36 поставочных уровней.

Второе слагаемое света движка — байт ``light`` записи сектора мира
(``0x5670d8 + 0x18*i + 0x16``).  Сумма берётся в ``0x43ca32``..``0x43ca4c``::

    43ca32  movsx ecx, BYTE PTR [eax+0x70]   ; poly.brightness
    43ca3c  movsx eax, BYTE PTR [edx+0x74]   ; object.brightness_bias
    43ca48  movsx eax, BYTE PTR [edx+0x16]   ; sector.light      <-- этот аудит
    43ca40/43ca4c  add ecx,eax               ; затем + тексель лайтмапа, clamp

Поле есть **только в рантайме**: загрузчик ``.sbw`` (``0x42ad9c``..``0x42ae05``)
читает из файла сектора ``u32 n_objects``, ``u16 kind``, ``u8 sub`` и
обнуляет ``+0x16`` (``0x42ae05``: ``mov BYTE PTR [ecx+0x16],0``).  Писателей
ровно три — скриптовые команды таблицы ``0x4b6784``:

===========  ===================================  ==========================
опкод        команда                              запись
===========  ===================================  ==========================
9            ``SET_SECTOR_BRIGHTNESS``            ``0x4578f8`` (обработчик ``0x4578d6``)
28           ``SET_SOURCESECTOR_BRIGHTNESS``      ``0x45807c`` (``0x458063``)
41           ``SET_HANDLESECTOR_BRIGHTNESS``      ``0x4583f1`` (``0x4583a6``)
===========  ===================================  ==========================

Во всех трёх значение — **байт операнда** ``mov cl,[code+pc]``, т.е. младший
байт 4-байтного ``L``-операнда, а читатель (``0x43ca48``) расширяет его
**знаково** (``movsx``): ``0xb8`` = ``-72``, ``0x80`` = ``-128``.
``SET_HANDLESECTOR_BRIGHTNESS`` идёт по всей таблице ``SECTOR_HANDLE``
(``0x5b1df8`` теги, ``0x617fb8`` секторы, счётчик ``0x5e3de0``) и пишет
каждый сектор с совпавшим тегом; ``SET_SOURCESECTOR_BRIGHTNESS`` берёт
сектор **источника** (``edi``, ``0x45807c``) — статически не разрешается.

Инструмент (только чтение; ``gm/sbt.py``, ``gm/sbtvm.py``, ``gm/sbw.py``,
``gm/sbtvm_exec.py``):

* находит все записи семейства света в ``SCRIPT`` (тип 11) каждого уровня:
  опкод, операнды, значения, цель (сектор / тег хэндла / источник);
* разрешает теги ``SET_HANDLESECTOR_BRIGHTNESS`` через таблицу
  ``SECTOR_HANDLE`` (тип 17), проверяет диапазон индексов у
  ``SET_SECTOR_BRIGHTNESS`` (геттер ``0x42d8a5``: ``0 <= i < 0x5670dc``);
* классифицирует контекст: блок с ``offset 0`` — главный скрипт (грузовой
  прогон, ``0x457390(0)``), остальные блоки — обработчики строк таблицы
  сущностей, они исполняются только по ``GENERATE``; отдельно считает, стоит
  ли в том же блоке ``ADD_*`` до команды света;
* прогоняет главный скрипт через ``gm.sbtvm_exec`` и проверяет, что при
  загрузке команды света **не исполняются**;
* сверяет с ``gm/sbw.py``: у записи сектора в файле полей только
  ``kind/sub/objects`` (байта света нет), и ни в одном ``.sbt`` нет записи
  ``SECTOR_STUFF`` (тип 8), т.е. файлу нечем задать ``+0x16``;
* считает, какие секторы скрипт вообще трогает и какая доля граней/площади
  уровня в них лежит.

Детерминизм: фиксированный порядок уровней, блоков и целей; никакого времени
в выводе. ``--json`` побайтно одинаков при повторном запуске.

Использование::

    python3 tools/re/sector_light.py                  # все уровни, сводка
    python3 tools/re/sector_light.py --level level0   # подробно по уровню
    python3 tools/re/sector_light.py --json > light.json
    python3 tools/re/sector_light.py --self-test
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm                  # noqa: E402
from gm import sbtvm_exec             # noqa: E402
from gm import sbw                    # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")
SCRIPT_RECORD = 11
SECTOR_HANDLE_RECORD = 17
SECTOR_STUFF_RECORD = 8

#: команды, пишущие ``sector+0x16``: имя -> (опкод, адрес записи, как берётся цель)
SECTOR_LIGHT = {
    "SET_SECTOR_BRIGHTNESS": (9, "0x4578f8", "sector"),
    "SET_SOURCESECTOR_BRIGHTNESS": (28, "0x45807c", "source"),
    "SET_HANDLESECTOR_BRIGHTNESS": (41, "0x4583f1", "handle"),
}
SECTOR_LIGHT_OPS = {op: name for name, (op, _a, _k) in SECTOR_LIGHT.items()}

#: команды, пишущие ``poly+0x70`` (яркость полигона) — тот же «свет из скрипта»,
#: но не второй слагаемый сектор; считаются отдельно, чтобы отделить одно от
#: другого
POLY_LIGHT = {
    "SET_POLY_BRIGHTNESS": 16,
    "SET_SOURCEPOLY_BRIGHTNESS": 33,
    "SET_HANDLEPOLY_BRIGHTNESS": 47,
}
POLY_LIGHT_OPS = {op: name for name, op in POLY_LIGHT.items()}

#: маска enable-строки таблицы сущностей (docs/ENTITY_FIELDS.md §1)
ROW_ENABLE = 0x1FF
ROW_ONESHOT = 0x200


# ---------------------------------------------------------------------------
# мелочи
# ---------------------------------------------------------------------------

def s32(value):
    return value - 0x100000000 if value >= 0x80000000 else value


def s8(value):
    return value - 0x100 if value >= 0x80 else value


def lit(value):
    """``-72(0xffffffb8)`` — как в ``gm.sbtvm.render``."""
    if value < 0:
        return "%d(0x%x)" % (value, value & 0xFFFFFFFF)
    return str(value)


def frac(part, whole):
    return round(part / whole, 4) if whole else 0.0


def resolve_game(path):
    if path:
        return path
    if os.path.isdir(MIRROR):
        return MIRROR
    return DEFAULT_GAME


# ---------------------------------------------------------------------------
# SCRIPT: строки таблицы сущностей, блоки, контекст
# ---------------------------------------------------------------------------

def script_rows(script):
    """``[(flags, id, offset)]`` — три колонки таблицы сущностей (``0x5b0df0``)."""
    n = script.n
    if n <= 0 or len(script.handles) != 12 * n:
        return []
    cols = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
    return [(cols[i], s32(cols[n + i]), cols[2 * n + i]) for i in range(n)]


def block_ranges(insns):
    """``[(start, end)]`` подскриптов верхнего уровня: блок кончается на ``0xFF``."""
    out, start = [], None
    for i in insns:
        if start is None:
            start = i.off
        if i.name == "END":
            out.append((start, i.off + 1))
            start = None
    return out


def block_instructions(insns):
    """``{start: [insn, ...]}`` для верхнего уровня (``IF``-тела не раскрываются)."""
    out, cur = {}, None
    for i in insns:
        if cur is None:
            cur = []
        cur.append(i)
        if i.name == "END":
            out[cur[0].off] = cur
            cur = None
    return out


def walk(insns):
    """Все инструкции, включая тела ``IF``/``ELSE``."""
    for i in insns:
        yield i
        if isinstance(i.body, list):
            yield from walk(i.body)


def block_of(offset, starts):
    """Начало подскрипта, содержащего *offset* (``starts`` отсортированы)."""
    if not starts:
        return offset
    best = starts[0]
    for s in starts:
        if s <= offset:
            best = s
        else:
            break
    return best


ADD_PREFIX = "ADD_"


def add_context(block_insns, offset):
    """Сколько ``ADD_*`` в блоке стоит до *offset* и есть ли они вообще."""
    before = 0
    total = 0
    for i in walk(block_insns):
        if not i.name.startswith(ADD_PREFIX):
            continue
        total += 1
        if i.off < offset:
            before += 1
    return before, total


# ---------------------------------------------------------------------------
# прогон главного скрипта (offset 0) через исполнитель
# ---------------------------------------------------------------------------

def load_run(script):
    """Исполнить главный скрипт (``0x457390(0)``) и вернуть счётчики.

    При загрузке уровня игра запускает подскрипт ``offset 0``; обработчики
    строк таблицы сущностей исполняются только по ``GENERATE``.  Прогон
    проверяет это на реальном байткоде: сколько команд света попало в
    исполнение главного скрипта.
    """
    try:
        state = sbtvm_exec.state_from_script(script)
        ex = sbtvm_exec.Executor(state, on_unknown="both",
                                 follow_generate=False)
        results = ex.run(0)
    except Exception as exc:                                  # noqa: BLE001
        return {"ok": False, "error": str(exc), "paths": 0, "steps": 0,
                "reasons": {}, "sector_light": 0, "poly_light": 0}
    counts = ex.command_counts
    return {
        "ok": True,
        "paths": len(results),
        "steps": sum(r.steps for r in results),
        "reasons": {r.reason: sum(1 for x in results if x.reason == r.reason)
                    for r in sorted(results, key=lambda x: x.reason)},
        "sector_light": sum(counts.get(op, 0) for op in SECTOR_LIGHT_OPS),
        "poly_light": sum(counts.get(op, 0) for op in POLY_LIGHT_OPS),
    }


# ---------------------------------------------------------------------------
# один уровень
# ---------------------------------------------------------------------------

def analyze_level(stem, sbt_path, sbw_path):
    """Полный разбор одного уровня (только чтение)."""
    tree = sbtmod.parse_file(sbt_path)
    script = tree.records_of(SCRIPT_RECORD)[0].payload
    insns, end = sbtvm.decode(script.code)
    if end != len(script.code):
        raise ValueError("%s: decode stopped at 0x%x of 0x%x"
                         % (stem, end, len(script.code)))

    rows = script_rows(script)
    by_offset = {}
    for flags, eid, off in rows:
        by_offset.setdefault(off, []).append({"flags": flags, "id": eid})

    handles = tree.records_of(SECTOR_HANDLE_RECORD)
    handle_rows = []
    if handles:
        table = handles[0].payload
        handle_rows = list(zip(table.tags, table.sectors))

    blocks = block_ranges(insns)
    starts = [b[0] for b in blocks]
    block_map = block_instructions(insns)

    # --- модель секторов из .sbw: их число, грани и площадь по секторам
    world = None
    sectors = []
    faces_by_sector = []
    refs_by_sector = []
    if sbw_path and os.path.isfile(sbw_path):
        world = sbw.parse_file(sbw_path)
        sectors = sorted(sbw.Sector.__dataclass_fields__)
        faces_by_sector = [sum(len(o.polys) for o in s.objects) for s in world.sectors]
        refs_by_sector = [sum(len(p.indices) for o in s.objects for p in o.polys)
                          for s in world.sectors]
    n_sectors = len(faces_by_sector) if world is not None else None

    sector_cmds = []
    poly_cmds = []
    for insn in walk(insns):
        if insn.name not in SECTOR_LIGHT_OPS.values() and \
                insn.name not in POLY_LIGHT_OPS.values():
            continue
        bstart = block_of(insn.off, starts)
        add_before, add_total = add_context(block_map.get(bstart, []), insn.off)
        owner = by_offset.get(bstart, [])
        entry = {
            "name": insn.name,
            "op": insn.op,
            "offset": insn.off,
            "block": bstart,
            "context": "init" if bstart == starts[0] else "event",
            "row_ids": [r["id"] for r in owner],
            "row_flags": [r["flags"] for r in owner],
            "add_before": add_before,
            "add_total": add_total,
            "operands": [a for a in insn.args],
        }
        if insn.name in POLY_LIGHT_OPS.values():
            value = s8(insn.args[-1] & 0xFF)
            entry["value"] = value
            entry["target"] = {"kind": "poly"}
            poly_cmds.append(entry)
            continue

        _op, _store, kind = SECTOR_LIGHT[insn.name]
        value = s8(insn.args[-1] & 0xFF)
        entry["value"] = value
        entry["value_raw"] = insn.args[-1] & 0xFF
        entry["upper_zero"] = (insn.args[-1] & 0xFFFFFFFF) >> 8 == 0
        if kind == "sector":
            index = insn.args[0]
            entry["index"] = index
            entry["target"] = {"kind": "sector", "sectors": [index]}
            entry["in_range"] = (n_sectors is None) or (0 <= index < n_sectors)
        elif kind == "handle":
            tag = insn.args[0]
            hit = sorted({sec for tg, sec in handle_rows if tg == tag})
            entry["tag"] = tag
            entry["target"] = {"kind": "handle", "tag": tag, "sectors": hit}
            entry["in_range"] = bool(hit) and (
                n_sectors is None or all(0 <= sec < n_sectors for sec in hit))
        else:
            entry["target"] = {"kind": "source", "sectors": []}
            entry["in_range"] = None
        sector_cmds.append(entry)

    # --- какие секторы скрипт вообще адресует и сколько граней в них
    touched = set()
    for cmd in sector_cmds:
        if cmd["in_range"] is False:      # геттер 0x42d8a5 отбрасывает индекс
            continue
        touched.update(cmd["target"].get("sectors") or [])
    touched = sorted(touched)
    faces_in = sum(faces_by_sector[i] for i in touched if i < len(faces_by_sector))
    refs_in = sum(refs_by_sector[i] for i in touched if i < len(refs_by_sector))
    faces_total = sum(faces_by_sector)
    refs_total = sum(refs_by_sector)

    # --- последовательности значений по цели внутри одного обработчика:
    #     сбрасывается ли эффект в 0 до конца блока
    sequences = []
    for bstart in sorted(block_map):
        per_target = {}
        for insn in block_map[bstart]:
            for i in walk([insn]):
                if i.name not in SECTOR_LIGHT_OPS.values():
                    continue
                if i.name == "SET_SECTOR_BRIGHTNESS":
                    key = ("sector", i.args[0])
                elif i.name == "SET_HANDLESECTOR_BRIGHTNESS":
                    key = ("handle", i.args[0])
                else:
                    key = ("source", None)
                per_target.setdefault(key, []).append(s8(i.args[-1] & 0xFF))
        for key in sorted(per_target, key=lambda k: (k[0], -1 if k[1] is None else k[1])):
            vals = per_target[key]
            sequences.append({
                "block": bstart,
                "target": {"kind": key[0], "ref": key[1]},
                "values": vals,
                "nonzero": sum(1 for v in vals if v),
                "ends_at_zero": vals[-1] == 0,
            })

    level = {
        "level": stem,
        "sbt": sbt_path,
        "sbw": sbw_path,
        "script": {"code_bytes": len(script.code), "rows": len(rows),
                   "blocks": len(blocks), "things": script.count},
        "world": {
            "sectors": n_sectors,
            "objects": world.n_objects if world is not None else None,
            "faces": world.n_polys if world is not None else None,
            "sector_fields": sectors if world is not None else None,
            "sector_light_in_file": False if world is not None else None,
            "sector_stuff_record": bool(tree.records_of(SECTOR_STUFF_RECORD)),
        },
        "counts": {name: sum(1 for c in sector_cmds if c["name"] == name)
                   for name in SECTOR_LIGHT},
        "poly_counts": {name: sum(1 for c in poly_cmds if c["name"] == name)
                        for name in POLY_LIGHT},
        "sector_commands": sector_cmds,
        "poly_commands": poly_cmds,
        "touched_sectors": touched,
        "faces_in_touched": faces_in,
        "faces_frac": frac(faces_in, faces_total),
        "area_in_touched": refs_in,
        "area_frac": frac(refs_in, refs_total),
        "sequences": sequences,
        "load_run": load_run(script),
    }
    return level


# ---------------------------------------------------------------------------
# сводка по всем уровням
# ---------------------------------------------------------------------------

def aggregate(levels):
    counts = {name: 0 for name in SECTOR_LIGHT}
    poly_counts = {name: 0 for name in POLY_LIGHT}
    values = {}
    contexts = {"init": 0, "event": 0}
    targets = {"sector": 0, "handle": 0, "source": 0}
    upper_zero = {True: 0, False: 0}
    out_of_range = 0
    add_before = 0
    add_total = 0
    enabled = 0
    disabled = 0
    resolved = 0
    load_light = 0
    load_ok = 0
    for lv in levels:
        for name, n in lv["counts"].items():
            counts[name] += n
        for name, n in lv["poly_counts"].items():
            poly_counts[name] += n
        for cmd in lv["sector_commands"]:
            values[cmd["value"]] = values.get(cmd["value"], 0) + 1
            contexts[cmd["context"]] += 1
            targets[cmd["target"]["kind"]] += 1
            upper_zero[cmd["upper_zero"]] += 1
            if cmd["in_range"] is False:
                out_of_range += 1
            if cmd["add_before"]:
                add_before += 1
            if cmd["add_total"]:
                add_total += 1
            if any(f & ROW_ENABLE for f in cmd["row_flags"]) or not cmd["row_flags"]:
                enabled += 1
            else:
                disabled += 1
            if cmd["target"].get("sectors"):
                resolved += 1
        if lv["load_run"]["ok"]:
            load_ok += 1
        load_light += lv["load_run"]["sector_light"]

    sequences = [s for lv in levels for s in lv["sequences"]]
    nonzero = [s for s in sequences if s["nonzero"]]
    summary = {
        "levels": len(levels),
        "counts": counts,
        "poly_counts": poly_counts,
        "sector_light_total": sum(counts.values()),
        "poly_light_total": sum(poly_counts.values()),
        "contexts": contexts,
        "targets": targets,
        "values": {str(k): values[k] for k in sorted(values)},
        "upper_zero": {"zero": upper_zero[True], "sign_extended": upper_zero[False]},
        "out_of_range": out_of_range,
        "resolved_commands": resolved,
        "add_before": add_before,
        "add_in_block": add_total,
        "rows_enabled": enabled,
        "rows_disabled": disabled,
        "load_run_ok": load_ok,
        "load_run_light": load_light,
        "sequences": len(sequences),
        "sequences_nonzero": len(nonzero),
        "sequences_end_zero": sum(1 for s in nonzero if s["ends_at_zero"]),
        "sequences_end_nonzero": sum(1 for s in nonzero if not s["ends_at_zero"]),
    }
    summary["levels_with_commands"] = sum(
        1 for lv in levels if sum(lv["counts"].values()))
    summary["sectors_touched"] = sum(len(lv["touched_sectors"]) for lv in levels)
    return summary


def analyze(game, level=None):
    if level:
        stem = os.path.basename(level)
        if stem.lower().endswith(".sbt"):
            stem = stem[:-4]
        sbt_path = level if os.path.isfile(level) else os.path.join(game, stem + ".sbt")
        if not os.path.isfile(sbt_path):
            raise SystemExit("level not found: %s" % level)
        sbw_path = os.path.join(game, stem + ".sbw")
        levels = [analyze_level(stem, sbt_path, sbw_path if os.path.isfile(sbw_path) else None)]
    else:
        levels = []
        for path in sorted(glob.glob(os.path.join(game, "*.sbt"))):
            stem = os.path.basename(path)[:-4]
            sbw_path = os.path.join(game, stem + ".sbw")
            levels.append(analyze_level(
                stem, path, sbw_path if os.path.isfile(sbw_path) else None))
    return {
        "game": game,
        "levels": levels,
        "summary": aggregate(levels),
        "code": {
            "sector_record": "0x5670d8, stride 0x18, light at +0x16 (u8, read movsx)",
            "readers": ["0x43ca48 (movsx byte [edx+0x16])",
                        "0x43ca32 poly+0x70", "0x43ca3c obj+0x74"],
            "loader_zero": "0x42ae05 (mov byte [ecx+0x16],0)",
            "writers": {name: {"op": op, "store": store, "target": kind}
                        for name, (op, store, kind) in SECTOR_LIGHT.items()},
            "operand_table": "0x4b6784",
            "sector_getter": "0x42d8a5 (0 <= i < 0x5670dc)",
            "handle_table": "SECTOR_HANDLE (type 17): tags 0x5b1df8, sectors 0x617fb8, n 0x5e3de0",
        },
    }


# ---------------------------------------------------------------------------
# вывод
# ---------------------------------------------------------------------------

def _fmt_values(values):
    """``-128,-72,0,16`` — значения по возрастанию, без счётчиков."""
    return ",".join(str(k) for k in sorted(int(k) for k in values))


def _fmt_hist(values):
    """``-128 x53,0 x362`` — гистограмма значений по возрастанию."""
    return ",".join("%d x%d" % (k, values[str(k)])
                    for k in sorted(int(k) for k in values))


def print_level(lv):
    print("== %s ==" % lv["level"])
    print("  .sbt %s" % lv["sbt"])
    print("  .sbw %s" % (lv["sbw"] or "нет"))
    sc = lv["script"]
    print("  SCRIPT: %d блоков, %d строк сущностей, %d байт кода"
          % (sc["blocks"], sc["rows"], sc["code_bytes"]))
    w = lv["world"]
    if w["sectors"] is None:
        print("  .sbw: нет пары — число секторов и доля граней неизвестны")
    else:
        print("  .sbw: %d секторов, %d объектов, %d граней; поля записи сектора: %s"
              % (w["sectors"], w["objects"], w["faces"], ",".join(w["sector_fields"])))
        print("  байта света в файле сектора: нет; запись SECTOR_STUFF (8): %s"
              % ("есть" if w["sector_stuff_record"] else "нет"))
    lr = lv["load_run"]
    if lr["ok"]:
        print("  прогон главного скрипта (offset 0): %d путь(ей), %d шагов, "
              "%s; команд света: сектор %d, полигон %d"
              % (lr["paths"], lr["steps"],
                 ",".join("%s x%d" % kv for kv in lr["reasons"].items()),
                 lr["sector_light"], lr["poly_light"]))
    else:
        print("  прогон главного скрипта: ошибка (%s)" % lr.get("error"))
    print("  команд света сектора: 9=%d  28=%d  41=%d"
          % tuple(lv["counts"][n] for n in SECTOR_LIGHT))
    print("  команд света полигона: 16=%d  33=%d  47=%d"
          % tuple(lv["poly_counts"][n] for n in POLY_LIGHT))
    print("  секторов адресовано: %d%s (граней %.1f%%, площади %.1f%%)"
          % (len(lv["touched_sectors"]),
             "/%d" % w["sectors"] if w["sectors"] is not None else "",
             100.0 * lv["faces_frac"], 100.0 * lv["area_frac"]))
    if lv["sector_commands"]:
        print("  записи:")
        for cmd in lv["sector_commands"]:
            tgt = cmd["target"]
            if tgt["kind"] == "source":
                where = "сектор источника (рантайм)"
            elif tgt["kind"] == "handle":
                where = "тег %d -> секторы %s" % (tgt["tag"], tgt["sectors"] or "нет")
            else:
                where = "сектор %d" % cmd["index"]
            print("    +0x%-5x %-28s %-5s %s = %-5d %s%s%s"
                  % (cmd["offset"], cmd["name"],
                     "init" if cmd["context"] == "init" else "event",
                     where, cmd["value"], lit(cmd["operands"][-1]),
                     "" if cmd["in_range"] is not False else "  [вне диапазона]",
                     "" if not cmd["add_before"] else "  [ADD_* до: %d]" % cmd["add_before"]))


def print_report(result):
    s = result["summary"]
    print("=== свет из скриптов: sector+0x16 (SET_SECTOR_BRIGHTNESS и родня) ===")
    print("игра: %s" % result["game"])
    print("уровней: %d (с командами света сектора: %d)"
          % (s["levels"], s["levels_with_commands"]))
    print()
    print("-- что доказано кодом --")
    print("  сектор мира 0x5670d8, шаг 0x18; light — u8 в +0x16, читается movsx")
    print("  сумма: 0x43ca32 poly+0x70 + 0x43ca3c obj+0x74 + 0x43ca48 sector+0x16")
    print("  загрузчик .sbw обнуляет поле: 0x42ae05")
    print("  писатели (таблица операндов 0x4b6784):")
    for name, (op, store, kind) in SECTOR_LIGHT.items():
        print("    %-2d %-28s %s  цель: %s" % (op, name, store, kind))
    print("  значение = младший байт L-операнда (mov cl,[code+pc]), знаковый")
    print()
    print("-- прогон при загрузке (главный скрипт, offset 0) --")
    print("  уровней с удачным прогоном: %d/%d; исполнено команд света сектора: %d"
          % (s["load_run_ok"], s["levels"], s["load_run_light"]))
    print()
    print("-- по уровням --")
    print("%-9s %5s %5s %5s %10s %8s %8s  %s"
          % ("уровень", "9", "28", "41", "секторы", "грани", "площадь", "значения"))
    for lv in result["levels"]:
        vals = {}
        for cmd in lv["sector_commands"]:
            vals[str(cmd["value"])] = vals.get(str(cmd["value"]), 0) + 1
        nsect = lv["world"]["sectors"]
        print("%-9s %5d %5d %5d %10s %7.1f%% %7.1f%%  %s"
              % (lv["level"], lv["counts"]["SET_SECTOR_BRIGHTNESS"],
                 lv["counts"]["SET_SOURCESECTOR_BRIGHTNESS"],
                 lv["counts"]["SET_HANDLESECTOR_BRIGHTNESS"],
                 "%d/%d" % (len(lv["touched_sectors"]), nsect) if nsect else "?",
                 100.0 * lv["faces_frac"], 100.0 * lv["area_frac"],
                 _fmt_values(vals) or "-"))
    print("%-9s %5d %5d %5d"
          % ("ИТОГО", s["counts"]["SET_SECTOR_BRIGHTNESS"],
             s["counts"]["SET_SOURCESECTOR_BRIGHTNESS"],
             s["counts"]["SET_HANDLESECTOR_BRIGHTNESS"]))
    print()
    print("-- значения (знаковый байт, записанный в sector+0x16) --")
    print("  всего команд света сектора: %d (init %d / event %d)"
          % (s["sector_light_total"], s["contexts"]["init"], s["contexts"]["event"]))
    print("  цели: сектор %d, тег хэндла %d, источник %d"
          % (s["targets"]["sector"], s["targets"]["handle"], s["targets"]["source"]))
    print("  операнд: верхние байты нулевые %d, записан со знаком (0xff…) %d"
          % (s["upper_zero"]["zero"], s["upper_zero"]["sign_extended"]))
    print("  вне диапазона секторов: %d" % s["out_of_range"])
    if s["sector_light_total"]:
        print("  гистограмма: %s" % _fmt_hist(s["values"]))
        neg = sum(v for k, v in s["values"].items() if int(k) < 0)
        zero = s["values"].get("0", 0)
        pos = sum(v for k, v in s["values"].items() if int(k) > 0)
        print("  отрицательных %d (%.1f%%), нулей %d (%.1f%%), положительных %d (%.1f%%)"
              % (neg, 100.0 * neg / s["sector_light_total"],
                 zero, 100.0 * zero / s["sector_light_total"],
                 pos, 100.0 * pos / s["sector_light_total"]))
    print()
    print("-- контекст и порядок --")
    print("  строк таблицы сущностей: enabled %d, disabled %d (команд)"
          % (s["rows_enabled"], s["rows_disabled"]))
    print("  команд в блоке с ADD_*: %d; из них ADD_* стоит до команды света: %d"
          % (s["add_in_block"], s["add_before"]))
    print("  цель-последовательностей в одном обработчике: %d, с ненулевым значением %d"
          % (s["sequences"], s["sequences_nonzero"]))
    print("  из них заканчиваются нулём: %d, ненулём: %d"
          % (s["sequences_end_zero"], s["sequences_end_nonzero"]))
    print()
    print("-- свет полигонов (poly+0x70, не второй слагаемый сектора) --")
    print("  16=%d  33=%d  47=%d; всего %d"
          % (s["poly_counts"]["SET_POLY_BRIGHTNESS"],
             s["poly_counts"]["SET_SOURCEPOLY_BRIGHTNESS"],
             s["poly_counts"]["SET_HANDLEPOLY_BRIGHTNESS"],
             s["poly_light_total"]))
    print()
    print("-- файл и рендер --")
    print("  в записи сектора .sbw поля света нет (только kind/sub/objects);")
    print("  запись SECTOR_STUFF (тип 8) отсутствует во всех 36 .sbt,")
    print("  поэтому при загрузке sector+0x16 = 0 у всех секторов всех уровней.")
    print("  Вывод: скриптовый свет в поставочных уровнях ИСПОЛЬЗУЕТСЯ, но только")
    print("  как событийный эффект (вспышка/затемнение), а не как освещение уровня;")
    print("  статический кадр gm/render.py (brightness + ambient) корректен,")
    print("  второй слагаемый вклад на загрузке равен нулю.")


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _synth_sbw(n_sectors=2, faces=((1, 1), (1, 0))):
    """Минимальный ``.sbw``: 0 текстур/эффектов, объекты и по 0..1 полигону."""
    out = bytearray()
    out += struct.pack("<I", 0)                        # textures
    out += struct.pack("<I", 0)                        # effects
    out += struct.pack("<I", len(faces))
    hdr = struct.pack("<3d", 1.0, 2.0, 3.0) + struct.pack(
        "<9d", 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    for si, (n_obj, n_poly) in enumerate(faces):
        out += struct.pack("<IHB", n_obj, si * 6, 0)   # n_objects, kind, sub
        for ob in range(n_obj):
            out += struct.pack("<I", ob) + hdr
    for si, (n_obj, n_poly) in enumerate(faces):
        for _ob in range(n_obj):
            out += struct.pack("<HH", 3, n_poly)
            for k in range(3):
                out += struct.pack("<3f", float(k), float(k + 1), 0.0)
            out += struct.pack("<fBBII", 10.0, 0, 0, 0, 0)      # radius/f75/f74/f7c/f78
            out += struct.pack("<3f", 1.0, 2.0, 3.0)            # bounds
            out += b"\0" * 24                                   # reserved
            for _p in range(n_poly):
                poly = struct.pack("<5H", 1, 0, 0, 0, 0)        # tex/lm/mat/ov/f7a
                poly += bytes((8, 3))                           # brightness, n_idx
                poly += struct.pack("<3H", 0, 1, 2)             # indices
                poly += struct.pack("<6H", 0, 0, 0, 0, 0, 0)    # scroll/scale/uv
                poly += struct.pack("<2I", 0, 0)
                poly += struct.pack("<18f", *([0.0] * 18))
                poly += struct.pack("<3H", 0, 0, 0)             # f74/f76/handle
                poly += struct.pack("<I", 0)                    # reserved
                poly += struct.pack("<I", 0x8000)               # flags
                poly += struct.pack("<3H", 0, 0, 0)             # anim
                assert len(poly) == 124 + 2 * 3, len(poly)
                out += poly
    return bytes(out)


def _synth_sbt(code, rows):
    """``.sbt`` с таблицей ``SECTOR_HANDLE`` (теги/секторы) и одним ``SCRIPT``."""
    out = bytearray(sbtmod.MAGIC + b"\0" * (80 - len(sbtmod.MAGIC))
                    + struct.pack("<I", 0))
    tags = [5, 7]
    secs = [0, 1]
    payload = struct.pack("<I", len(tags))
    payload += struct.pack("<%dI" % len(tags), *tags)
    payload += struct.pack("<%dI" % len(secs), *secs)
    out += bytes((17,)) + payload
    script = sbtmod.Script(
        count=0, n=len(rows), m=len(code), flag=0,
        things=b"",
        handles=(b"".join(struct.pack("<I", r[0] & 0xFFFFFFFF) for r in rows)
                 + b"".join(struct.pack("<I", r[1] & 0xFFFFFFFF) for r in rows)
                 + b"".join(struct.pack("<I", r[2] & 0xFFFFFFFF) for r in rows)),
        code=code, tables=b"")
    out += bytes((11,)) + script.encode()
    out += bytes((255,))
    return bytes(out)


def _synth_code():
    """Байткод: init-блок с командой 9 и event-блок с 28/41/41/9/16."""
    code = bytearray()
    code += bytes((9,)) + struct.pack("<iI", 1, 0xFFFFFFC8)      # sector 1, -56
    code += bytes((0xFF,))
    event_off = len(code)
    # ADD_LOCATION 'ELLLAAA': E=4, LLL=12, AAA=24 -> 40 байт операндов
    code += bytes((196,)) + struct.pack("<i", 1) + b"\0" * 36
    assert len(code) - event_off == 41, len(code) - event_off
    code += bytes((28,)) + struct.pack("<i", 32)
    code += bytes((41,)) + struct.pack("<iI", 5, 0xFFFFFF80)     # tag 5 -> sector 0
    code += bytes((41,)) + struct.pack("<iI", 7, 0)              # tag 7 -> sector 1
    code += bytes((9,)) + struct.pack("<iI", 99, 200)            # вне диапазона
    code += bytes((16,)) + struct.pack("<4i", 0, 0, 0, 64)       # poly light
    code += bytes((0xFF,))
    return bytes(code), event_off


def self_test():
    ok = 0
    bad = 0

    def check(name, cond, detail=""):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("ok    %s" % name)
        else:
            bad += 1
            print("FAIL  %s  (%s)" % (name, detail))

    tmp = tempfile.mkdtemp(prefix="sector-light-")
    try:
        code, event_off = _synth_code()
        rows = [(ROW_ENABLE, -30026, event_off)]
        with open(os.path.join(tmp, "synth.sbt"), "wb") as fh:
            fh.write(_synth_sbt(code, rows))
        with open(os.path.join(tmp, "synth.sbw"), "wb") as fh:
            fh.write(_synth_sbw())

        parsed = sbtmod.parse_file(os.path.join(tmp, "synth.sbt"))
        check("синтетический .sbt разбирается", len(parsed.records) == 3,
              [r.name for r in parsed.records])
        decoded, end = sbtvm.decode(parsed.records_of(11)[0].payload.code)
        check("синтетический байткод декодируется до конца", end == len(code))

        result = analyze(tmp, "synth")
        lv = result["levels"][0]
        cmds = lv["sector_commands"]
        by_name = {}
        for c in cmds:
            by_name.setdefault(c["name"], []).append(c)
        check("найдены 5 команд света сектора", len(cmds) == 5, len(cmds))
        check("9/28/41 посчитаны",
              lv["counts"] == {"SET_SECTOR_BRIGHTNESS": 2,
                               "SET_SOURCESECTOR_BRIGHTNESS": 1,
                               "SET_HANDLESECTOR_BRIGHTNESS": 2},
              lv["counts"])
        init = [c for c in cmds if c["context"] == "init"]
        check("init-контекст определён (block 0)", len(init) == 1
              and init[0]["block"] == 0 and init[0]["value"] == -56,
              init)
        ev = [c for c in cmds if c["context"] == "event"]
        check("event-контекст определён", len(ev) == 4
              and all(c["block"] == event_off for c in ev), ev)
        check("значение = знаковый младший байт",
              by_name["SET_HANDLESECTOR_BRIGHTNESS"][0]["value"] == -128
              and by_name["SET_SOURCESECTOR_BRIGHTNESS"][0]["value"] == 32)
        check("тег разрешается по SECTOR_HANDLE",
              by_name["SET_HANDLESECTOR_BRIGHTNESS"][0]["target"]["sectors"] == [0]
              and by_name["SET_HANDLESECTOR_BRIGHTNESS"][1]["target"]["sectors"] == [1])
        bad_cmd = [c for c in by_name["SET_SECTOR_BRIGHTNESS"] if c["index"] == 99][0]
        check("индекс вне диапазона помечен", bad_cmd["in_range"] is False)
        check("главный прогон исполнил init-команду 9",
              lv["load_run"]["ok"] and lv["load_run"]["sector_light"] == 1,
              lv["load_run"])
        check("ADD_* до команды света в event-блоке",
              all(c["add_before"] >= 1 for c in ev), [c["add_before"] for c in ev])
        check("свет полигона отделён от света сектора",
              lv["poly_counts"]["SET_POLY_BRIGHTNESS"] == 1
              and len(lv["poly_commands"]) == 1, lv["poly_counts"])
        check("сектор в файле: поля kind/sub/objects, света нет",
              lv["world"]["sector_fields"] == ["kind", "objects", "sub"]
              and lv["world"]["sector_light_in_file"] is False,
              lv["world"]["sector_fields"])
        check("доля граней адресованного сектора", lv["faces_in_touched"] == 1
              and lv["faces_frac"] == 1.0, lv["faces_frac"])

        sc = result["summary"]
        check("сводка: 5 команд, 1 init / 4 event",
              sc["sector_light_total"] == 5 and sc["contexts"] == {"init": 1, "event": 4},
              sc["contexts"])
        check("сводка: гистограмма значений",
              sc["values"] == {"-128": 1, "-56": 2, "0": 1, "32": 1}, sc["values"])
        check("сводка: 1 вне диапазона", sc["out_of_range"] == 1, sc["out_of_range"])

        first = json.dumps(analyze(tmp, "synth"), sort_keys=True, ensure_ascii=False)
        second = json.dumps(analyze(tmp, "synth"), sort_keys=True, ensure_ascii=False)
        check("детерминизм --json", first == second)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n%d тестов, %d провалено" % (ok + bad, bad))
    return 0 if not bad else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Скриптовый свет сектора (+0x16): аудит .sbt/.sbw (только чтение)")
    ap.add_argument("--game", default=None,
                    help="каталог игры (по умолчанию зеркало %s, иначе %s)"
                         % (MIRROR, DEFAULT_GAME))
    ap.add_argument("--level", default=None, metavar="NAME",
                    help="один уровень: stem или путь к .sbt")
    ap.add_argument("--json", action="store_true", help="вывод в JSON")
    ap.add_argument("--detail", action="store_true",
                    help="подробный разбор уровня(ей) после сводки")
    ap.add_argument("--self-test", action="store_true", help="синтетический тест")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    game = resolve_game(args.game)
    if not os.path.isdir(game):
        print("нет каталога игры: %s" % game, file=sys.stderr)
        return 2
    result = analyze(game, args.level)
    if args.json:
        json.dump(result, sys.stdout, sort_keys=True, ensure_ascii=False, indent=1)
        sys.stdout.write("\n")
        return 0
    if args.level:
        print_level(result["levels"][0])
        print()
    print_report(result)
    if args.detail and not args.level:
        print()
        for lv in result["levels"]:
            print_level(lv)
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
