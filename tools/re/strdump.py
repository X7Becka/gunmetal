#!/usr/bin/env python3
"""Print NUL-terminated strings from gundx.exe.

Usage:  python3 strdump.py 0x4b8f64 0x4b9074 ...        # by VMA
        python3 strdump.py --range 0x4b8c00 0x4b8f64    # every string in a range
"""
import sys

import exelib


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "--range":
        lo, hi = int(argv[1], 16), int(argv[2], 16)
        for va, s in exelib.strings_in(lo, hi):
            print("0x%06x  %r" % (va, s))
        return 0
    for a in argv:
        v = int(a, 16)
        print("0x%06x  %r" % (v, exelib.cstr(v)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
