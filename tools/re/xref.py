#!/usr/bin/env python3
"""Find little-endian dword references to a value (typically a string VMA).

Usage:  python3 xref.py 0x4b8fc0 0x4b8ff8 ...

Each hit is reported as a VMA (plus the string at the referenced address),
which is how the command/keyword tables and their handlers were located.
"""
import struct
import sys

import exelib


def refs(value):
    needle = struct.pack("<I", value)
    d = exelib.data()
    out = []
    i = d.find(needle)
    while i >= 0:
        out.append(i)
        i = d.find(needle, i + 1)
    return out


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    for a in argv:
        v = int(a, 16)
        hits = []
        for o in refs(v):
            va = exelib.off2va(o)
            hits.append("0x%06x" % va if va is not None else "file+0x%x" % o)
        print("0x%06x  %r  ->  %s" % (v, exelib.cstr(v), ", ".join(hits) or "-"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
