#!/usr/bin/env python3
"""Print a VMA range from the pre-generated objdump listing.

Usage:  python3 disrange.py START [END]        # hex VMAs, e.g. 450be0 450d60
        python3 disrange.py START --lines N

Generate the listing first (see ``tools/re/README.md``)::

    objdump -D -M intel "gundx.exe" > /tmp/opencode/gundx.asm
"""
import sys

import exelib


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    lo = int(argv[0], 16)
    hi = lo + 0x100
    lines = None
    if "--lines" in argv:
        lines = int(argv[argv.index("--lines") + 1])
    elif len(argv) > 1:
        hi = int(argv[1], 16)
    for line in exelib.disasm(lo, hi, lines):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
