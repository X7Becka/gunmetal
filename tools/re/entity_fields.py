#!/usr/bin/env python3
"""The scripted entity table of a level: three parallel columns and their row.

A level's behaviour is one ``SCRIPT`` record (``.sbt`` type 11).  At load time
``0x462a80`` copies its tail into three flat arrays of ``n`` ``u32``, plus the
compiled code buffer::

    u32 n                       -> 0x5b1df0        (count, loader cap 0x400)
    n * u32 flags               -> 0x5b0df0        column 0
    n * u32 id                  -> 0x5e4680        column 1
    n * u32 sub-script offset   -> 0x616658        column 2
    u32 m                       -> 0x5e2dd0        (code length)
    m bytes code                -> [0x4b6784]      (pointer to the blob)
    u8 flag                     -> (if set) nine 0x400-byte "sleeping script" tables

The table is **column-major**: row *i* is ``(flags[i], id[i], offset[i])`` --
12 logical bytes, but three separate 4-byte-stride arrays (that is why the
three bases are 0x4000 apart at the cap ``n = 0x400``).

What each column is (all three proven in the binary):

* **flags** -- an enable mask.  Bits 0..7 = players 0..7, bit 8 (``0x100``) =
  any non-player source, bit 9 (``0x200``) = *one-shot*.  The compiler writes
  ``0x1ff`` for ``EVENT``, ``0x3ff`` for ``ONESHOT`` and ``0`` for ``DISABLED``
  (``0x4607xx``/``0x4608xx``); ``ENABLE``/``DISABLE`` (cid 5/6, ``0x457c80`` /
  ``0x457c49``) touch ``0x1ff``; ``SOURCEPLAYER_ENABLE``/``_DISABLE`` (cid
  142/141, ``0x457cef``/``0x457c9e``) touch one bit (``1 << player``, or
  ``0x100`` for a non-player source); ``generate_runner`` (``0x45fa83``) tests
  ``1 << player`` and, when ``0x200`` is set, clears ``0x1ff`` after the run.
* **id** -- an *interned name*, not an index: the lexer (``0x45fb90`` ->
  ``0x45fea0``) maps a token to an id.  A numeric token keeps its value; a
  name found in the built-in table ``0x4acde8`` (26 records x 0x30) gets
  ``-index``; a new name gets ``-30000-index`` and is appended there.  So
  ``0..-25`` are the system events, ``-30026..`` are the level's own names,
  and positive ids (``1000000`` ...) are numeric literals.  All rows whose id
  equals the event run, in table order.
* **offset** -- the byte offset of that entity's **sub-script** inside the
  compiled code (the same buffer the VM runs through ``0x4b6784``); the block
  ends at the next ``0xFF``.  15 025/15 061 shipped blocks end exactly where
  the next row starts.

The row carries **only** the id, the enable mask and the sub-script.  The
entity's *content* -- thing type, position, angles, model -- is inside that
sub-script, in the ``ADD_THING*`` record the builder ``0x46d000`` reads::

    ADD_THING      (cid 1, 40-byte record)
      +0x00 u32  type            THING_TYPES index (0xFFFFFFFF = none)
      +0x04 i32  x, +0x08 y, +0x0c z
      +0x10/18/20 double  three angles
    ADD_THING_EX   (cid 2, 56-byte record; condition walker 0x45d120 case 2)
      +0x00 u32  game-mode mask  (filter 0x45c830)
      +0x04 i32  level_lo, +0x08 level_hi   (vs DAT_0083d62c)
      +0x0c u32  type, then x/y/z at +0x10..0x18, angles +0x1c..0x33,
      +0x34 i32  extra (-1/0)
    ADD_THING_AT_SOURCE / _WEAPON_POINT / _RELATIVE: position from the source

and the model is ``THING_TYPES[type] + 0x1a4`` -> ``SOB_TABLE`` (see
``docs/THING_FIELDS.md``).  Parent/child links and the sector are **not** in
the script table: they exist only in the runtime instance (``0x7ac080``:
parent ``+0x310``, child_count ``+0x314``, child_slots ``+0x318``, sector
``+0x3c8``), filled by the spawn; ``GENERATE_FOR_CHILDREN`` (cid 3, ``0x4575d3``)
and ``GENERATE_FOR_PARENT`` (cid 4) walk those links.

Full map, evidence and the C++/test coverage table: ``docs/ENTITY_FIELDS.md``.

Usage::

    python3 tools/re/entity_fields.py --map
    python3 tools/re/entity_fields.py --verify [--game DIR] [--level STEM]
    python3 tools/re/entity_fields.py --self-test
    python3 tools/re/entity_fields.py --verify --json

Read-only: the ``.sbt`` files are only parsed, never written.
"""
import argparse
import collections
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm                  # noqa: E402
from gm import scene as scenemod      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
SCRIPT_RECORD = 11
TYPE_RECORD = 4
MODEL_RECORD = 20
MODEL_OFF = 0x1A4
COUNT_CAP = 0x400                     # loader 0x462a80 rejects n > 0x400
THING_RECORD = 0x28                   # ADD_THING payload after the 2 op bytes
GENERATE = 2
ONESHOT = 0x200
PLAYER_BITS = 0x0FF
SYSTEM_BIT = 0x100
ENABLE_MASK = 0x1FF
MAX_NAMED = 0x1A                      # built-in event names in 0x4acde8
NAMED_BASE = 30000

ID_REFS = ("GENERATE", "GENERATE_FOR_CHILDREN", "GENERATE_FOR_PARENT",
           "ENABLE", "DISABLE",
           "SOURCEPLAYER_ENABLE", "SOURCEPLAYER_DISABLE")

# ---------------------------------------------------------------------------
# The map, as data (printed by --map, consumed by docs/ENTITY_FIELDS.md)
# ---------------------------------------------------------------------------

GLOBALS = [
    ("0x005b1df0", "u32", "entity count n (0..0x400; loader 0x462a80)",
     "0x00462b2d..0x00462d85 read/written, 0x00456f7b reset, "
     "0x0045fa34 loop bound"),
    ("0x005b0df0", "u32[0x400]", "flags column, stride 4",
     "writer 0x00460744/0x004607c5/0x00460843, reader 0x0045fa83, "
     "0x00457c52/0x00457c89/0x00457cde/0x00457d2d"),
    ("0x005e4680", "u32[0x400]", "id column, stride 4",
     "writer 0x00460781/0x004607f1/0x0046087c, reader 0x0045fa5c, "
     "0x00457c49/0x00457c80/0x00457cd5/0x00457d24"),
    ("0x00616658", "u32[0x400]", "sub-script offset column, stride 4",
     "writer 0x004608a0, reader 0x0045fabf (-> 0x00457390)"),
    ("0x005e2dd0", "u32", "compiled code length m (cap 0x30001)",
     "0x00462bd4, 0x00460ea0/0x004609f0 (compiler)"),
    ("0x004b6784", "u8 *", "pointer to the compiled code blob",
     "0x004573b1, 0x0045d250, 0x0046d000 callers"),
    ("0x005e3df0..0x005e41f0", "u32[0x100]", "sleeping-script: sub-script offset",
     "SLEEP cid 7 -> 0x0045c49e, tick 0x00457305, reset 0x00456f86"),
    ("0x005e4278", "u32[0x100]", "sleeping-script: remaining ticks",
     "0x0045c46c, 0x0045730f/0x00457319"),
    ("0x00616258", "u32[0x100]", "sleeping-script: parameter",
     "0x0045c4a5, 0x0045734b"),
    ("0x005e5e58", "u32[0x100]", "sleeping-script: parameter", "0x0045c476"),
    ("0x005e2dd8", "u32[0x100]", "sleeping-script: parameter", "0x0045c480"),
    ("0x005e39d8", "u32[0x100]", "sleeping-script: parameter", "0x0045c48a"),
    ("0x005e35d8", "u32[0x100]", "sleeping-script: parameter", "0x0045c494"),
    ("0x005e31d8", "u32[0x100]", "sleeping-script: parameter", "0x0045c4ac"),
    ("0x005b29c8", "u32[0x100]", "sleeping-script: player mask",
     "0x0045c4b3, STOP_... 0x0045b95e"),
]

ROW = [
    ("0", "u32", "flags", "enable mask: bits 0..7 players, 0x100 system, "
     "0x200 one-shot", "proven"),
    ("4", "u32", "id", "interned event name (-index / -30000-index / numeric)",
     "proven"),
    ("8", "u32", "offset", "sub-script offset in the compiled code", "proven"),
]

FLAG_BITS = [
    (0x001, "player 0 enabled", "0x45fa8f (1 << player)", "proven"),
    (0x002, "player 1 enabled", "0x45fa8f", "proven"),
    (0x004, "player 2 enabled", "0x45fa8f", "proven"),
    (0x008, "player 3 enabled", "0x45fa8f", "proven"),
    (0x010, "player 4 enabled", "0x45fa8f", "proven"),
    (0x020, "player 5 enabled", "0x45fa8f", "proven"),
    (0x040, "player 6 enabled", "0x45fa8f", "proven"),
    (0x080, "player 7 enabled", "0x45fa8f", "proven"),
    (0x100, "non-player source enabled", "0x45fad2 (TEST AH,1)", "proven"),
    (0x200, "one-shot: clear 0x1ff after the run",
     "0x45fa95 (TEST AH,2) + 0x45fa9e", "proven"),
]

ID_SPACE = [
    ("0", "SYSTEM_LEVEL_STARTUP", "0x45fb90: index 0 -> -0"),
    ("-1..-25", "SYSTEM_*/INTERNAL_* events (0x4acde8, 26 x 0x30)",
     "0x45fb90 index < 0x1a -> -index"),
    ("-30026..-30699", "level-defined names, interned in compile order",
     "0x45fb90 -> -30000-index (max 700 names)"),
    (">= 0 (1000000, ...)", "numeric literal id", "0x45fea0 keeps the number"),
]

ADD_THING_RECORD = [
    ("+0x00", "u32", "type", "THING_TYPES index; 0xFFFFFFFF = none", "proven"),
    ("+0x04", "i32", "x", "fild operand 2 of 0x46d000", "proven"),
    ("+0x08", "i32", "y", "fild operand 3", "proven"),
    ("+0x0c", "i32", "z", "fild operand 4", "proven"),
    ("+0x10", "double", "angle 0", "arg 5/6 of 0x46d000", "proven"),
    ("+0x18", "double", "angle 1", "arg 7/8", "proven"),
    ("+0x20", "double", "angle 2", "arg 9/10", "proven"),
    ("+0x24", "—", "next instruction", "record size 0x28 (0x45d2a5)", "proven"),
]

ADD_THING_EX_RECORD = [
    ("+0x00", "u32", "game-mode mask", "gate 0x45c830 (0x45d2eb); same as "
     "ADD_ITEM at 0x459adc", "proven"),
    ("+0x04", "i32", "level_lo", "cmp DAT_0083d62c (0x45d2fc)", "proven"),
    ("+0x08", "i32", "level_hi", "cmp DAT_0083d62c (0x45d300)", "proven"),
    ("+0x0c", "u32", "type", "G operand; 2662/2663 valid in the corpus",
     "proven"),
    ("+0x10..0x18", "3 x i32", "x, y, z", "absolute position", "proven"),
    ("+0x1c..0x33", "3 x double", "angles", "0.0 in the corpus", "hypothesis"),
    ("+0x34", "i32", "extra", "-1 / 0", "unknown"),
]

#: field -> C++ (Things / Ai / vm_exec) -> test.  ``state`` is one of
#: ``yes`` (ported and exercised), ``partial``, ``no`` (not ported).
FIELD_TO_CPP = [
    ("id", "VmEntity::id, Things::entity/parent", "test_vm_exec, test_things",
     "yes"),
    ("flags", "VmEntity::flags (raw), Things::flags", "test_vm_exec, "
     "test_things", "yes"),
    ("flags & 0x1ff", "VmEntity::enabled()", "test_vm_exec (ENABLE/DISABLE)",
     "yes"),
    ("flags bit 0..7 (players)", "VmExecutor::source_player_bit", "none",
     "no: C++ uses 1 << (9 + player); the engine uses 1 << player"),
    ("flags bit 8 (0x100)", "VmExecutor::source_player_bit fallback", "none",
     "partial: C++ treats 'no player' as 0x100 (correct), but a real player "
     "never maps to 0x100"),
    ("flags bit 9 (0x200 one-shot)",
     "not modelled (h_generate never clears 0x1ff)", "none",
     "no: a one-shot entity stays enabled and re-runs"),
    ("offset", "VmEntity::offset, Things::script", "test_vm_exec, test_things",
     "yes"),
    ("duplicate ids (same id in several rows)",
     "VmState::entities is a map id -> one row", "none",
     "no: the engine runs every matching row, C++ keeps the last"),
    ("type / model", "Things::type, type_name, model, model_index",
     "test_things, test_sbtvm", "yes"),
    ("position / angles", "Things::position, angles", "test_things", "yes"),
    ("parent / children", "Things::parent, children(), child_ids()",
     "test_things", "yes (script side; town-town links are runtime-only)"),
    ("sector", "absent (runtime instance +0x3c8)", "none",
     "no: computed at spawn from the position (0x4360f1)"),
    ("spawn order", "Things discovery order (context, block, instruction)",
     "test_things", "yes"),
    ("sleeping-script table", "not modelled", "none", "no"),
]

# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------


def s32(v):
    """View a ``u32`` as the signed value the engine compares (id, x, z)."""
    return v - 0x100000000 if v >= 0x80000000 else v


def parse_rows(script):
    """``[(flags, id, offset)]`` of the ``SCRIPT`` payload (column-major)."""
    n = script.n
    if n <= 0 or len(script.handles) != 12 * n:
        return []
    cols = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
    return [(cols[i], s32(cols[n + i]), cols[2 * n + i]) for i in range(n)]


def event_index(eid):
    """Index in the built-in name table ``0x4acde8`` for an event id, or None.

    Mirrors ``0x45f9d0``/``0x463a50``: ``id <= 0``; index ``-id`` for the 26
    built-ins, ``-id - 30000`` for level names, negative -> no name.
    """
    if eid > 0:
        return None
    i = -eid
    if i > MAX_NAMED - 1:
        i -= NAMED_BASE
    return i if i >= 0 else None


def flags_view(flags):
    """Decode one flags word into ``(players, system, oneshot, unknown)``."""
    players = [p for p in range(8) if flags & (1 << p)]
    return {
        "players": players,
        "system": bool(flags & SYSTEM_BIT),
        "oneshot": bool(flags & ONESHOT),
        "unknown": flags & ~(ENABLE_MASK | ONESHOT),
    }


def blocks(insns):
    """Split a top-level instruction list into ``(start, end)`` sub-script blocks."""
    out, start = [], None
    for i in insns:
        if start is None:
            start = i.off
        if i.name == "END":
            out.append((start, i.off + 1))
            start = None
    return out


def block_map(insns):
    """``{block start: [instruction, ...]}`` for the top level of a script."""
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
    """Every instruction, descending into ``IF``/``ELSE`` bodies."""
    for i in insns:
        yield i
        body = getattr(i, "body", None)
        if isinstance(body, list):
            yield from walk(body)


def decode_script(script):
    """``(top_level, blocks)`` of a ``SCRIPT`` payload, ``([], [])`` on error."""
    try:
        top, _end = sbtvm.decode(script.code)
    except Exception:                                    # noqa: BLE001
        return [], []
    return top, blocks(top)


def thing_types(sbt):
    return [t.name for t in sbt.records_of(TYPE_RECORD)[0].payload.entries]


def thing_models(sbt):
    return [e.name for e in sbt.records_of(MODEL_RECORD)[0].payload.entries]


def model_index_of(sbt, type_index):
    """``SOB_TABLE`` index stored at ``THING_TYPES[type_index] + 0x1a4``."""
    entry = sbt.records_of(TYPE_RECORD)[0].payload.entries[type_index]
    rel = (MODEL_OFF - 0x198) // 4                      # first u32 of tail
    return struct.unpack_from("<%dI" % (len(entry.tail) // 4), entry.tail)[rel]


def thing_records(insns):
    """``(kind, args)`` of every ``ADD_THING*`` condition in *insns*."""
    out = []
    for i in insns:
        if i.name == "END":
            break
        if i.op != 0xC9 or not i.args:
            continue
        name, args = i.args[0]
        if name.startswith("ADD_THING"):
            out.append((i.off, name, args))
    return out


def thing_type_of(name, args):
    """The ``G`` operand of an ``ADD_THING*`` record (or None)."""
    sig = None
    for _cid, (nm, s) in sbtvm.CONDITIONS.items():
        if nm == name:
            sig = s
            break
    if not sig or "G" not in sig or len(args) <= sig.index("G"):
        return None
    return s32(args[sig.index("G")])


def id_references(insns):
    """``[(name, id)]`` of every command that addresses an entity by id."""
    out = []
    for i in walk(insns):
        if i.op == 0xC9 or i.name == "END" or not i.args:
            continue
        if i.name in ID_REFS and isinstance(i.args[0], int):
            out.append((i.name, s32(i.args[0])))
    return out


def scene_ids(insns):
    """Ids of doors and switches (the other owners of the same id space)."""
    doors, switches = set(), set()
    for i in walk(insns):
        if i.op == 0xC9 or i.name == "END" or not i.args:
            continue
        if i.name in ("ADD_DOOR", "ADD_DOOR_AT_SOURCE") and len(i.args) > 5:
            doors.update(s32(v) for v in i.args[1:6] if isinstance(v, int))
        elif i.name == "ADD_SWITCH" and len(i.args) > 1:
            switches.add(s32(i.args[1]))
    return doors, switches


# ---------------------------------------------------------------------------
# Verification over the shipped levels
# ---------------------------------------------------------------------------

STATS = [
    ("levels", 36),
    ("rows", 15061),
    ("blocks_ending_on_next_row", 15025),
    ("type_table", 2896),
    ("spawns", 391),
]


def verify_level(path):
    """Everything one ``.sbt`` says about its entity table (read-only)."""
    sbt = sbtmod.parse_file(path)
    script = sbt.records_of(SCRIPT_RECORD)[0].payload
    top, blks = decode_script(script)
    insns = list(walk(top))
    rows = parse_rows(script)
    ids = {r[1] for r in rows}
    starts = {b[0] for b in blks}
    type_names = thing_types(sbt)
    models = thing_models(sbt)
    doors, switches = scene_ids(insns)

    # 1. payload layout (writer 0x462db0 -> loader 0x462a80)
    layout_ok = (len(script.things) == 40 * script.count and
                 len(script.handles) == 12 * script.n and
                 len(script.code) == script.m and
                 (script.flag != 0 or not script.tables) and
                 (script.flag == 0 or len(script.tables) == 9 * 0x400))

    # 2. flags are exactly the three compile-time masks
    flag_hist = collections.Counter(r[0] for r in rows)
    flags_ok = set(flag_hist) <= {0x000, ENABLE_MASK, ENABLE_MASK | ONESHOT}

    # 3. every offset is a block start, and the block ends on the next row
    bad_offset, chained, with_next = [], 0, 0
    for k, (_f, eid, off) in enumerate(rows):
        if off not in starts:
            bad_offset.append((eid, off))
            continue
        if k + 1 < len(rows):
            with_next += 1
            nxt = rows[k + 1][2]
            for a, b in blks:
                if a == off:
                    if b == nxt:
                        chained += 1
                    break
        elif off in starts:
            chained += 0                             # last row: open-ended
    offsets_ok = not bad_offset

    # 4. thing types + models (the ADD_THING* records live in the sub-scripts)
    n_types = n_bad = n_null = 0
    bmap = block_map(top)
    for _f, _eid, off in rows:
        for _site, name, args in thing_records(walk(bmap.get(off, ()))):
            t = thing_type_of(name, args)
            if t is None:
                continue
            n_types += 1
            if t == -1:
                n_null += 1
            elif 0 <= t < len(type_names):
                idx = model_index_of(sbt, t)
                if not 0 <= idx < len(models):
                    n_bad += 1
            else:
                n_bad += 1
    types_ok = n_bad == 0

    # 5. id references resolve (doors/switches share the id space)
    refs = id_references(insns)
    resolved = [1 for _n, e in refs if e in ids]
    unresolved = collections.Counter(
        (n, "door" if e in doors else "switch" if e in switches else "other")
        for n, e in refs if e not in ids)
    spawns = sum(1 for i in insns
                 if i.name in ("SET_PLAYER_START", "SET_DEATHMATCH_START"))

    # 6. cross-check against gm/scene.py (must be identical)
    scene_rows = scenemod.entities(script)          # [(id, flags, offset)]
    mine = [(r[1], r[0], r[2]) for r in rows]       # [(id, flags, offset)]
    scene_ok = mine == list(scene_rows)

    dup_rows = len(rows) - len(ids)
    return {
        "level": os.path.splitext(os.path.basename(path))[0],
        "n": script.n,
        "unique_ids": len(ids),
        "duplicate_rows": dup_rows,
        "flags": dict(sorted(flag_hist.items())),
        "blocks": len(blks),
        "chained": chained,
        "with_next": with_next,
        "things_array": script.count,
        "types": n_types,
        "type_null": n_null,
        "type_bad": n_bad,
        "type_table": len(type_names),
        "refs": len(refs),
        "refs_resolved": len(resolved),
        "refs_unresolved": {"%s/%s" % (k[0], k[1]): v
                            for k, v in sorted(unresolved.items())},
        "spawns": spawns,
        "code": script.m,
        "sleep_tables": bool(script.flag),
        "checks": {
            "layout": layout_ok,
            "flags": flags_ok,
            "offsets": offsets_ok,
            "types": types_ok,
            "scene_parity": scene_ok,
        },
        "bad_offsets": bad_offset[:5],
        "row0": rows[0] if rows else None,
    }


def verify(game, level=None, quiet=False):
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if level:
        files = [p for p in files
                 if os.path.splitext(os.path.basename(p))[0] == level]
    if not files:
        return None, ("no .sbt level %r in %s" % (level, game) if level
                      else "no .sbt in %s" % game)
    per = [verify_level(p) for p in files]
    agg = collections.Counter()
    flags = collections.Counter()
    unresolved = collections.Counter()
    for d in per:
        agg["levels"] += 1
        agg["rows"] += d["n"]
        agg["chained"] += d["chained"]
        agg["with_next"] += d["with_next"]
        agg["types"] += d["types"]
        agg["type_table"] += d["type_table"]
        agg["type_null"] += d["type_null"]
        agg["refs"] += d["refs"]
        agg["refs_resolved"] += d["refs_resolved"]
        agg["spawns"] += d["spawns"]
        agg["duplicate_rows"] += d["duplicate_rows"]
        agg["blocks"] += d["blocks"]
        agg["things_array"] += d["things_array"]
        flags.update(d["flags"])
        for k, v in d["refs_unresolved"].items():
            unresolved[k] += v
        for k, v in d["checks"].items():
            agg["check_" + k] += 1 if v else 0
    return {"per_level": per, "total": dict(agg),
            "flags": dict(sorted(flags.items())),
            "unresolved": dict(unresolved)}, None


# ---------------------------------------------------------------------------
# C++ parity (read-only source scan)
# ---------------------------------------------------------------------------

def cpp_parity():
    """Compare the proven flag mapping with engine/src/res/vm_exec.cpp."""
    src = os.path.join(ROOT, "engine", "src", "res", "vm_exec.cpp")
    hdr = os.path.join(ROOT, "engine", "src", "world", "things.h")
    if not os.path.exists(src):
        return []
    text = open(src, encoding="utf-8").read()
    out = []
    out.append(("flags & 0x1FF in vm_exec.cpp", "0x1FF" in text))
    out.append(("one-shot 0x200 handled in vm_exec.cpp",
                bool(re.search(r"0x200u?", text))))
    m = re.search(r"source_player_bit\(const VmState& state\)\s*\{(.*?)\n\}",
                  text, re.S)
    ok = bool(m and re.search(r"1u\s*<<\s*\(\s*9\s*\+", m.group(1)))
    out.append(("player bit = 1 << (9 + player) (engine: 1 << player)", ok))
    htext = open(hdr, encoding="utf-8").read() if os.path.exists(hdr) else ""
    out.append(("Things::parent/children in things.h",
                "children(int parent)" in htext))
    return out


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

class Failure(AssertionError):
    pass


def _ck(cond, msg):
    if not cond:
        raise Failure(msg)


def _synthetic():
    """A hand-built SCRIPT record exercising rows, flags, blocks and refs."""
    # block 0: EVAL ADD_THING(type=1, x=2, y=3, z=4, angles 0) ; END
    rec = struct.pack("<i", 1) + struct.pack("<3i", 2, 3, 4)
    rec += struct.pack("<3d", 0.0, 0.0, 0.0)
    block0 = b"\xc9\x01" + rec + b"\xff"
    # block 1: GENERATE(-30026) ; ENABLE(-30026) ; DISABLE(-30026) ; END
    block1 = (b"\x02" + struct.pack("<i", -30026) +
              b"\x05" + struct.pack("<i", -30026) +
              b"\x06" + struct.pack("<i", -30026) + b"\xff")
    code = block0 + block1
    rows = [(0x1FF, s32(0xFFFFFFFF & -30026), 0),
            (0x3FF, s32(0xFFFFFFFF & -30026), len(block0)),
            (0x000, 0, len(block0) + len(block1) - 1)]
    n = len(rows)
    cols = [struct.pack("<%dI" % n, *[r[k] & 0xFFFFFFFF for r in rows])
            for k in range(3)]
    script = sbtmod.Script(count=0, n=n, m=len(code), flag=0,
                           things=b"", handles=b"".join(cols),
                           code=code, tables=b"")
    parsed = parse_rows(script)
    _ck(parsed == [(0x1FF, -30026, 0), (0x3FF, -30026, len(block0)),
                   (0x000, 0, len(block0) + len(block1) - 1)],
        "parse_rows split the columns wrong: %r" % (parsed,))
    _ck(len(script.encode()) == 4 + 4 + 12 * n + 4 + len(code) + 1,
        "Script.encode() length drifted from the loader layout")

    top, blks = decode_script(script)
    _ck(blks == [(0, len(block0)), (len(block0), len(code))],
        "block split wrong: %r" % (blks,))
    things = thing_records(top)
    _ck(len(things) == 1 and things[0][1] == "ADD_THING", "ADD_THING not found")
    _ck(thing_type_of(things[0][1], things[0][2]) == 1, "type operand wrong")
    refs = id_references(top)
    _ck(refs == [("GENERATE", -30026), ("ENABLE", -30026),
                 ("DISABLE", -30026)], "id references wrong: %r" % (refs,))

    # flags view: players 0..7 + system, one-shot
    v = flags_view(0x1FF)
    _ck(v["players"] == list(range(8)) and v["system"] and not v["oneshot"],
        "0x1ff decode wrong: %r" % (v,))
    v = flags_view(0x3FF)
    _ck(v["oneshot"] and v["unknown"] == 0, "0x3ff decode wrong: %r" % (v,))
    _ck(flags_view(0)["players"] == [] and not flags_view(0)["system"],
        "0 must mean disabled")

    # id space
    _ck(event_index(0) == 0, "id 0 is SYSTEM_LEVEL_STARTUP (index 0)")
    _ck(event_index(-25) == 25, "id -25 is the last built-in")
    _ck(event_index(-30026) == 26, "-30026 is the first level name")
    _ck(event_index(-30065) == 65, "-30065 is level name 65")
    _ck(event_index(1000000) is None, "positive ids are numeric literals")

    # a mis-sized payload must not be silently accepted
    bad = sbtmod.Script(count=0, n=2, m=0, flag=0, things=b"",
                        handles=b"\x00" * 4, code=b"", tables=b"")
    _ck(parse_rows(bad) == [], "truncated handles must yield no rows")
    return "synthetic: %d bytes, %d rows, %d blocks, %d refs" % (
        len(code), len(parsed), len(blks), len(refs))


def _real(game):
    """Totals over the shipped corpus; skipped when the game is absent."""
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if not files:
        return "real: SKIP (no .sbt in %s)" % game
    report, err = verify(game)
    _ck(err is None, "verify failed: %s" % err)
    t = report["total"]
    want = dict(STATS)
    _ck(t["levels"] == want["levels"], "levels %d != %d" % (t["levels"],
                                                          want["levels"]))
    _ck(t["rows"] == want["rows"], "rows %d != %d" % (t["rows"], want["rows"]))
    _ck(t["chained"] == want["blocks_ending_on_next_row"],
        "chained %d != %d" % (t["chained"], want["blocks_ending_on_next_row"]))
    _ck(t["type_table"] == want["type_table"],
        "type table %d != %d" % (t["type_table"], want["type_table"]))
    _ck(t["types"] == 7563, "thing sites %d != 7563" % t["types"])
    _ck(t["spawns"] == want["spawns"], "spawns %d != %d" % (t["spawns"],
                                                          want["spawns"]))
    _ck(set(report["flags"]) <= {0, 0x1FF, 0x3FF},
        "unexpected flags in the corpus: %r" % (report["flags"],))
    for k in ("layout", "flags", "offsets", "types", "scene_parity"):
        _ck(t["check_" + k] == t["levels"],
            "check %s failed for %d levels" % (k, t["levels"] -
                                               t["check_" + k]))
    # one pinned example per campaign/DM file
    dm1 = [d for d in report["per_level"] if d["level"] == "dm1"][0]
    lv0 = [d for d in report["per_level"] if d["level"] == "level0"][0]
    _ck(dm1["n"] == 363 and dm1["duplicate_rows"] == 8,
        "dm1 rows/duplicates drifted: %r" % (dm1["n"],))
    _ck(lv0["n"] == 350, "level0 rows drifted: %r" % (lv0["n"],))
    _ck(dm1["row0"] == (0x1FF, -1, 0), "dm1 row 0 drifted: %r" % (dm1["row0"],))
    return ("real: %d levels, %d rows (%d duplicate), %d blocks chained, "
            "%d THING_TYPES, %d GENERATE/ENABLE refs (%d resolve), "
            "%d spawns, flags %r" %
            (t["levels"], t["rows"], t["duplicate_rows"], t["chained"],
             t["type_table"], t["refs"], t["refs_resolved"], t["spawns"],
             report["flags"]))


def self_test(game):
    print("== entity_fields self-test ==")
    ok = 0
    try:
        print("  ok   " + _synthetic())
        ok += 1
    except Failure as exc:
        print("  FAIL synthetic: %s" % exc)
    try:
        print("  ok   " + _real(game))
        ok += 1
    except Failure as exc:
        print("  FAIL real: %s" % exc)
    par = cpp_parity()
    for name, passed in par:
        print("  info cpp: %-58s %s" % (name, "yes" if passed else "no"))
    print("self-test: %d/2 passed" % ok)
    return 0 if ok == 2 else 1


# ---------------------------------------------------------------------------
# Printing
# ---------------------------------------------------------------------------

def print_map():
    print("SCRIPT record (type 11) -> runtime arrays")
    print("  u32 c; c*40 things; u32 n; 3 columns x n*4; u32 m; m code;"
          " u8 flag; [9*0x400]")
    print("\nGlobals")
    for addr, typ, what, who in GLOBALS:
        print("  %-22s %-10s %-42s %s" % (addr, typ, what, who))
    print("\nRow (column-major, stride 4)")
    for off, typ, name, what, st in ROW:
        print("  +%-3s %-4s %-8s %-55s %s" % (off, typ, name, what, st))
    print("\nFlags bits")
    for bit, what, who, st in FLAG_BITS:
        print("  0x%03x %-40s %-28s %s" % (bit, what, who, st))
    print("\nId space (0x45fb90 / 0x45fea0 / 0x463a50)")
    for rng, what, how in ID_SPACE:
        print("  %-18s %-52s %s" % (rng, what, how))
    print("\nADD_THING record (cid 1, 0x45d240, 40 bytes)")
    for off, typ, name, what, st in ADD_THING_RECORD:
        print("  %-10s %-9s %-10s %-34s %s" % (off, typ, name, what, st))
    print("\nADD_THING_EX record (cid 2, 0x45d120 case 2, 56 bytes)")
    for off, typ, name, what, st in ADD_THING_EX_RECORD:
        print("  %-12s %-11s %-10s %-36s %s" % (off, typ, name, what, st))
    print("\nField -> C++ -> test")
    for field, cpp, test, state in FIELD_TO_CPP:
        print("  %-34s %-52s %-30s %s" % (field, cpp, test, state))


def print_verify(report, level=None):
    per = report["per_level"]
    print("%-9s %5s %5s %5s %-22s %7s %8s %7s %6s"
          % ("level", "n", "uniq", "dup", "flags", "chained", "things",
             "refs", "spawns"))
    for d in per:
        flags = ",".join("%03x" % k for k in d["flags"])
        print("%-9s %5d %5d %5d %-22s %4d/%-4d %8d %4d/%-3d %6d"
              % (d["level"], d["n"], d["unique_ids"], d["duplicate_rows"],
                 flags, d["chained"], d["with_next"], d["types"],
                 d["refs_resolved"], d["refs"], d["spawns"]))
    t = report["total"]
    print("\ntotal: %d levels, %d rows (%d duplicate ids), %d THING_TYPES, "
          "%d ADD_THING* sites"
          % (t["levels"], t["rows"], t["duplicate_rows"], t["type_table"],
             t["types"]))
    print("  blocks ending on the next row: %d/%d"
          % (t["chained"], t["with_next"]))
    print("  id references: %d, resolved in the table: %d (%.1f%%)"
          % (t["refs"], t["refs_resolved"],
             100.0 * t["refs_resolved"] / max(t["refs"], 1)))
    print("  unresolved by owner: %r" % (report["unresolved"],))
    print("  flags histogram: %r" % (report["flags"],))
    print("  spawns (SET_PLAYER_START + SET_DEATHMATCH_START): %d" % t["spawns"])
    print("  things array in the record: %d records (0 = scene is scripted)"
          % t["things_array"])
    checks = [k for k in ("layout", "flags", "offsets", "types",
                          "scene_parity")]
    print("  checks: " + ", ".join("%s %d/%d" % (c, t["check_" + c],
                                                t["levels"])
                                   for c in checks))
    bad = [(d["level"], d["bad_offsets"]) for d in per if d["bad_offsets"]]
    if bad:
        print("  BAD offsets: %r" % (bad[:5],))
    par = cpp_parity()
    if par:
        print("  C++ parity (engine/src):")
        for name, passed in par:
            print("    %-58s %s" % (name, "yes" if passed else "no"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--map", action="store_true", help="print the field map")
    ap.add_argument("--verify", action="store_true",
                    help="check the map against real .sbt levels (read-only)")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic + real checks, non-zero exit on failure")
    ap.add_argument("--game", default=os.environ.get("GM_GAME", DEFAULT_GAME),
                    help="game directory")
    ap.add_argument("--level", help="only this level stem (e.g. dm1)")
    ap.add_argument("--json", action="store_true", help="machine-readable dump")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test(args.game)
    if not (args.verify or args.map or args.json):
        args.map = True

    code = 0
    if args.map:
        print_map()
    if args.verify or args.json:
        report, err = verify(args.game, args.level)
        if err:
            print("entity_fields: %s" % err, file=sys.stderr)
            return 2
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print()
            print_verify(report)
            for d in report["per_level"]:
                if not all(d["checks"].values()):
                    code = 1
    return code


if __name__ == "__main__":
    sys.exit(main())
