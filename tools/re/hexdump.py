#!/usr/bin/env python3
"""Hex + ASCII dump of a VMA range (or of a raw executable file offset).

Usage:  python3 hexdump.py 0x4b8f60 0x130
        python3 hexdump.py --off 0xa1600 0x40
"""
import sys

import exelib


def dump(blob, base_label):
    for off in range(0, len(blob), 16):
        chunk = blob[off:off + 16]
        hexs = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print("%s+%04x  %-47s  %s" % (base_label, off, hexs, text))


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "--off":
        off = int(argv[1], 16)
        n = int(argv[2], 16) if len(argv) > 2 else 0x40
        dump(exelib.data()[off:off + n], "file")
        return 0
    va = int(argv[0], 16)
    n = int(argv[1], 16) if len(argv) > 1 else 0x40
    o = exelib.va2off(va)
    if o is None:
        print("0x%x: unmapped" % va)
        return 1
    dump(exelib.data()[o:o + n], "0x%06x" % va)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
