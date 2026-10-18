#!/usr/bin/env python3
"""Dump the ``DEFINE_*`` script tables of a ``.sbt`` with their field map.

The file record layout comes from the signature strings; the *memory* layout
comes from the consumers (``docs/FORMATS.md`` 12.3)::

    DEFINE_WEAPONS   0x450be0  record 0x2a8 bytes @ 0x80fa00
    DEFINE_AMMO      0x450b10  record 0x38  bytes @ 0x818960
    DEFINE_EQUIPMENT 0x450e60  record 0x40  bytes @ 0x814c8c

Weapon fields proven so far: dword 2 = ammo type index into DEFINE_AMMO,
dword 3 = ammo spent per shot, dword 8 = price, dword 10 = flag bits.

Usage:  python3 sbt_fields.py LEVEL.sbt [--table DEFINE_WEAPONS] [--all]
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm                  # noqa: E402

# file dword index -> (memory offset, field name) for DEFINE_WEAPONS.
# "code" = proven from the engine code, "val" = inferred from value ranges;
# see docs/FORMATS.md 12.3 for the evidence.
WEAPON_MAP = {
    0: ("+0x24", "fire mode"),
    1: ("+0x2c", "fire period"),
    2: ("+0x3c", "ammo type index"),
    3: ("+0x40", "ammo per shot"),
    4: ("+0x30", "projectile id"),
    5: ("+0x34", "alt projectile id"),
    6: ("+0x38", "state value"),
    7: ("+0x4c", "id"),
    8: ("+0x44", "price"),
    9: ("+0x48", "score"),
    10: ("+0x28", "flags"),
    11: ("+0x294", "class index"),
    12: ("+0x00", "ballistic factor * 0.91"),
    13: ("+0x00", "ballistic factor * 0.91"),
    14: ("+0x08", "spread 1 (tan)"),
    15: ("+0x0c", "spread 1 hi"),
    16: ("+0x10", "spread 2 (tan)"),
    17: ("+0x14", "spread 2 hi"),
    18: ("+0x18", "range/speed"),
    19: ("+0x1c", "range/speed hi"),
    20: ("+0x20", "muzzle node index"),
}


def expand(tail_sig, vals):
    """``[(dword_index, letter, dword)]`` for a parsed operand tail."""
    out = []
    k = 0
    for ch, val in zip(tail_sig, vals):
        size = sbtvm.LETTER_SIZE[ch]
        if size == 8:
            lo, hi = struct.unpack_from("<2I", struct.pack("<d", val))
            out.append((k, ch, lo))
            out.append((k + 1, ch, hi))
            k += 2
        elif size == "str":
            continue
        else:
            out.append((k, ch, val & 0xFFFFFFFF))
            k += 1
    return out


def dump_table(ins, show_all):
    name = ins.name
    sig = sbtvm.TABLE_SIGS.get(ins.op, "")
    nstr = len(sig) - len(sig.lstrip("S"))
    tail = sig[nstr:]
    print("%s  %d entries  (record 0x%x bytes, %d strings)" % (
        name, len(ins.data), sbtvm.record_size(sig), nstr))
    for idx, (strs, vals) in enumerate(ins.data):
        print("  [%d] %s" % (idx, " | ".join(strs)))
        dwords = expand(tail, vals)
        show_all = show_all or name != "DEFINE_WEAPONS"
        for k, ch, dw in dwords:
            if name == "DEFINE_WEAPONS":
                off, label = WEAPON_MAP.get(k, ("?", ""))
            else:
                off, label = "d%d" % k, ""
            if label or show_all:
                print("      %-6s %-6s %-12s %s" % (
                    "d%d" % k, off, "%d" % dw, label))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path")
    ap.add_argument("--table", help="only this table (e.g. DEFINE_WEAPONS)")
    ap.add_argument("--all", action="store_true",
                    help="also print unlabelled fields")
    opts = ap.parse_args(argv)

    sbt = sbtmod.parse(open(opts.path, "rb").read())
    for rec in sbt.records_of(11):
        for ins in sbtvm.decode(rec.payload.code)[0]:
            if ins.name == "DEFINE_LEVEL_OBJECTIVES":
                if opts.table and opts.table != ins.name:
                    continue
                print("DEFINE_LEVEL_OBJECTIVES %d entries" % len(ins.args))
                for s in ins.args:
                    print("    %r" % s)
            elif ins.name == "SOURCEPLAYER_PATH":
                if opts.table and opts.table != ins.name:
                    continue
                print("SOURCEPLAYER_PATH n=%d %s" % (ins.args[0][1], ins.data))
            elif ins.op in sbtvm.TABLE_SIGS:
                if opts.table and opts.table != ins.name:
                    continue
                dump_table(ins, opts.all)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
