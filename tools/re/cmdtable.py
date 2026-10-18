#!/usr/bin/env python3
"""Dump the engine's two command tables.

    COMMANDS    0x4b53e8, 192 entries, stride 20  -> opcode = id (1..0xca)
    CONDITIONS  0x4b62e8,  59 entries, stride 20   -> id emitted after 0xc9/IF

Each entry is ``{name_ptr, sig_ptr, flag, -1, id}``; the signature letters and
their sizes are documented in ``docs/FORMATS.md`` 12.3.  The same data is
embedded in ``gm/sbtvm.py`` (regenerate with ``tools/gen_sbtvm.py``).

Usage:  python3 cmdtable.py [COMMANDS|CONDITIONS]
"""
import struct
import sys

import exelib

TABLES = (("COMMANDS", 0x4B53E8, 192), ("CONDITIONS", 0x4B62E8, 59))


def main(argv):
    only = argv[0].upper() if argv else None
    d = exelib.data()
    for name, base, count in TABLES:
        if only and only != name:
            continue
        print("=== %s  base=0x%x  %d entries ===" % (name, base, count))
        o = exelib.va2off(base)
        for i in range(count):
            nptr, sptr, flag, _neg1, cid = struct.unpack_from("<5I", d, o + 20 * i)
            print("%3d  0x%02x  %-36s sig=%-30s flag=%d" % (
                i, cid, exelib.cstr(nptr, 64) or "",
                exelib.cstr(sptr, 64) or "", flag))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
