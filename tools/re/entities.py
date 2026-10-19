#!/usr/bin/env python3
"""The level's entity table and what each entity is.

The ``SCRIPT`` record (``.sbt``, type 11) ends with three parallel ``u32``
columns of ``n`` rows -- the level's *entity table* (loader ``0x462b03``...
``0x462bb0``: flags -> ``0x5b0df0``, ids -> ``0x5e4680``, offsets -> ``0x616658``,
count -> ``0x5b1df0``)::

    row = {u32 flags; u32 id; u32 offset}

* **flags** -- bits 0..8 are the entity's state flags (``0x1ff``; ``ENABLE``
  (cid 5, ``0x457c67``) and ``DISABLE`` (cid 6, ``0x457c30``) set/clear them),
  bits 9..16 are the eight *players* (bit 9 = player 0: ``0x45d8bd`` does
  ``lea ecx,[edx+9]; shl ebp,cl``).
* **id** -- the compiler's interned name id; every command addresses the entity
  by it (linear search over the id column, ``0x457c49``/``0x45fa5c``).
* **offset** -- the offset of the entity's **sub-script inside the compiled
  code** (the same buffer the VM runs: ``ds:0x4b6784``, ``0x45d138``).  Proof:
  362/363 of dm1's blocks end exactly where the next row begins, and
  ``GENERATE`` runs a row's block with ``0x457390(offset, ...)`` (``0x45fa00``
  -> ``0x45fb01``).

``GENERATE`` (cid 2, operand ``E`` = an id -> ``0x4575a3`` -> ``0x479960``) both
switches the *door* carrying that id (``0x44f690``: state at ``[door+0x14]``,
sound ``0x2b11`` through ``0x46b8d0``) and runs the entity's sub-script.  The
sub-script is where a thing is set up: the ``ADD_THING*`` family (condition ids
1..5, handler ``0x45d240``/``0x45d881``...) calls the builder ``0x46d000``,
which takes the **thing type as the first field of a record in the same code
buffer** (``{u32 type; s32 x,y,z; u32 params...}``, ``0x45d283``..``0x45d2a0``)
and resolves ``THING_TYPES[type] + 0x1a4`` -> ``SOB_TABLE`` -> the model.  So

    GENERATE id -> sub-script -> ADD_THING*(type, ...) -> THING_TYPES -> model

is the full chain from a scripted entity to its geometry.

Checks over the shipped data: every ``ADD_THING*`` type operand is a valid
``THING_TYPES`` index (``tools/re/thing_models.py`` resolves the model), every
``GENERATE`` operand is an id present in the level's table, and the sub-script
blocks are contiguous.

Usage:  python3 entities.py [--game DIR] [--level STEM] [--rows N] [--json]
"""
import argparse
import bisect
import collections
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from gm import sbt as sbtmod            # noqa: E402
from gm import sbtvm                    # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MODEL_OFF = 0x1A4
GENERATE = 2


def thing_pos(name, sig):
    """Index of the thing-type operand in an ``ADD_THING*`` condition.

    The letter ``G`` marks it (``GLLLAAA``, ``LGAAA``, ``LLGDDDD``,
    ``LLLGLLLAAAL``, ``LGLLLAAADDDD``); only the ``ADD_THING*`` family builds a
    thing (handler ``0x45d240``/``0x45d881`` -> ``0x46d000``).
    """
    if not name.startswith("ADD_THING") or not sig or "G" not in sig:
        return None
    return sig.index("G")


def rows(script):
    """``[(flags, id, offset)]`` of the level's entity table."""
    n = script.n
    if n <= 0 or len(script.handles) < 12 * n:
        return []
    c = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
    s32 = lambda v: v - 0x100000000 if v >= 0x80000000 else v     # noqa: E731
    return [(c[i], s32(c[n + i]), c[2 * n + i]) for i in range(n)]


def types(sbt):
    return [t.name for t in sbt.records_of(4)[0].payload.entries]


def models(sbt):
    return [e.name for e in sbt.records_of(20)[0].payload.entries]


def split_blocks(insns):
    """Split a top-level instruction list into sub-script blocks.

    Every entity row's ``offset`` points at the first instruction of a block;
    the block ends at the next top-level ``END`` (0xFF).  Returning the segments
    once turns the per-entity decode into a flat lookup (the old code decoded
    the whole rest of the script for every row, 45k times in the supply).
    """
    segs, starts, cur = [], [], []
    for i in insns:
        cur.append(i)
        if i.name == "END":
            segs.append((cur[0].off, i.off + 1, cur))
            starts.append(cur[0].off)
            cur = []
    return segs, starts


def block_for(segs, starts, off):
    """The segment containing *off*, or ``None``."""
    i = bisect.bisect_right(starts, off) - 1
    if i >= 0 and segs[i][0] <= off < segs[i][1]:
        return segs[i]
    for seg in segs:
        if seg[0] <= off < seg[1]:
            return seg
    return None


def thing_types_of(ins):
    """``(condition name, type operand)`` of every ``ADD_THING*`` in a block."""
    out = []
    for i in ins:
        if i.name == "END":
            break
        if i.op != 0xC9:                      # only bare conditions create
            continue
        name, args = i.args[0]
        sig = sbtvm.CONDITIONS.get(_cond_id(name), (None, None))[1]
        pos = thing_pos(name, sig)
        if pos is not None and len(args) > pos and isinstance(args[pos], int):
            out.append((name, args[pos]))
    return out


def _cond_id(name):
    for cid, (nm, _s) in sbtvm.CONDITIONS.items():
        if nm == name:
            return cid
    return -1


def generate_targets(ins):
    """Ids passed to ``GENERATE`` inside one block."""
    out = []
    for i in ins:
        if i.name == "END":
            break
        if i.op == GENERATE and i.args:
            v = i.args[0]
            if isinstance(v, int):
                out.append(v & 0xFFFFFFFF)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--level", help="only this level stem (e.g. dm1)")
    ap.add_argument("--rows", type=int, default=12, help="rows printed per level")
    ap.add_argument("--json", action="store_true", help="dump everything as JSON")
    args = ap.parse_args(argv)

    files = sorted(glob.glob(os.path.join(args.game, "*.sbt")))
    if args.level:
        files = [p for p in files
                 if os.path.splitext(os.path.basename(p))[0] == args.level]
    if not files:
        print("no matching .sbt files in %s" % args.game, file=sys.stderr)
        return 1

    data = {}
    n_rows = tot_blocks = chained = 0
    type_ops = bad_type = null_type = 0
    gens = tot_gen = 0
    min_type = None
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        s = sbtmod.parse_file(path)
        script = s.records_of(11)[0].payload
        names = types(s)
        model = models(s)
        rows_ = rows(script)
        ids = {i & 0xFFFFFFFF for _f, i, _o in rows_}
        try:
            top, _end = sbtvm.decode(script.code)
        except Exception:
            top = []
        segs, starts = split_blocks(top)
        out = []
        for k, (fl, eid, off) in enumerate(rows_):
            seg = block_for(segs, starts, off) if off < len(script.code) else None
            end = seg[1] if seg else None
            tail = [i for i in seg[2] if i.off >= off] if seg else []
            nxt = rows_[k + 1][2] if k + 1 < len(rows_) else None
            tt = thing_types_of(tail)
            tg = generate_targets(tail)
            first = seg[2][0].name if seg else "?"
            n_rows += 1
            if end is not None:
                tot_blocks += 1
                if nxt is not None and end == nxt:
                    chained += 1
            for _nm, v in tt:
                type_ops += 1
                if v == 0xFFFFFFFF:           # a null reference, 3 in the data
                    null_type += 1
                elif isinstance(v, int) and 0 <= v < len(names):
                    min_type = v if min_type is None else min(min_type, v)
                else:
                    bad_type += 1
            for t in tg:
                tot_gen += 1
                if t in ids or t == 0xFFFFFFFF:
                    gens += 1
            out.append({"flags": fl, "id": eid, "offset": off, "end": end,
                        "first": first,
                        "thing_types": [[nm, v, names[v], model[_type_model(s, v)]]
                                        if isinstance(v, int) and 0 <= v < len(names)
                                        else [nm, v, None, None]
                                        for nm, v in tt],
                        "generates": tg})
        data[stem] = {"n": script.n, "types": len(names), "rows": out}

    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
        return 0

    for stem in sorted(data):
        d = data[stem]
        print("=== %s: сущностей %d, типов %d ===" % (stem, d["n"], d["types"]))
        for r in d["rows"][:args.rows]:
            tt = ", ".join("%s=%d(%s -> %s)" % (nm, v, tn, md)
                           for nm, v, tn, md in r["thing_types"]) or "-"
            gg = ", ".join("%d" % (t - 0x100000000 if t >= 0x80000000 else t)
                           for t in r["generates"]) or "-"
            print("   id=%-12d flags=0x%03x off=%-6s->%-6s %-9s"
                  " ADD_THING: %s | GENERATE: %s"
                  % (r["id"], r["flags"], r["offset"], r["end"], r["first"],
                     tt, gg))

    print("\nentities: %d rows in %d levels; blocks ending on the next row %d/%d"
          % (n_rows, len(data), chained, tot_blocks))
    print("  ADD_THING type operands: %d valid, %d null (0xffffffff), %d invalid"
          " (min index %s)"
          % (type_ops - bad_type - null_type, null_type, bad_type, min_type))
    print("  GENERATE ids in the entity table: %d/%d" % (gens, tot_gen))
    return 0


def _type_model(sbt, idx):
    """SOB_TABLE index of ``THING_TYPES[idx]`` (see tools/re/thing_models.py)."""
    tt = sbt.records_of(4)[0].payload.entries[idx]
    rel = (MODEL_OFF - 0x198) // 4          # tail[3]
    v = struct.unpack_from("<%dI" % (len(tt.tail) // 4), tt.tail)[rel]
    return v


if __name__ == "__main__":
    sys.exit(main())
