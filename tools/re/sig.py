#!/usr/bin/env python3
"""Byte size of operand signatures, plus the raw letter-class table.

Usage:  python3 sig.py SSSLLLLEEEELLLLDBBDL SLEOLLLLLLL ...
        python3 sig.py --table            # raw bytes at 0x457204
"""
import sys

import exelib

CLASSES = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
LETTER_TABLE = 0x457204


def show_table():
    chunk = exelib.data()[exelib.va2off(LETTER_TABLE):][:len(CLASSES)]
    print("table 0x%x (index = letter - 'A'):" % LETTER_TABLE)
    for i, b in enumerate(chunk):
        size = exelib.LETTER_SIZE.get(CLASSES[i], "invalid")
        print("  %s -> class 0x%02x  %s" % (CLASSES[i], b, size))


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "--table":
        show_table()
        return 0
    for sig in argv:
        parts = []
        for ch in sig:
            size = exelib.LETTER_SIZE[ch]
            parts.append("%s=%s" % (ch, size))
        print("%-40s %4d = 0x%x   (%s)" % (
            sig, exelib.sig_size(sig), exelib.sig_size(sig), " ".join(parts)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
