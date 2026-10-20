#!/usr/bin/env python3
"""Regenerate ``gm/sou2.py``'s embedded gain table from the C++ decoder.

The table is a verbatim image of ``gundx.exe``'s ``0x852340..0x852ac0``; the
C++ reader is the source of truth (it was extracted from the PE and verified
against the engine).  Keeping a generator makes the provenance obvious and
avoids hand-editing 1920 bytes.

    python3 tools/gen_sou2_table.py           # rewrite gm/sou2.py
    python3 tools/gen_sou2_table.py --check   # exit 1 if out of date
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CPP = ROOT / "engine" / "src" / "res" / "sou2.cpp"
PY = ROOT / "gm" / "sou2.py"


def extract_table() -> bytes:
    text = CPP.read_text()
    match = re.search(r"kGainTable\s*=\s*\{(.*?)\n\};", text, re.S)
    if not match:
        raise SystemExit("kGainTable not found in engine/src/res/sou2.cpp")
    values = [int(tok, 16) for tok in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1))]
    if len(values) != 0x780:
        raise SystemExit(f"expected 0x780 table bytes, found {len(values)}")
    return bytes(values)


def render(table: bytes) -> str:
    lines = []
    for i in range(0, len(table), 32):
        lines.append(table[i:i + 32].hex())
    body = "\n".join('    "' + line + '"' for line in lines)
    return "GAIN_TABLE = bytes.fromhex(\n" + body + "\n)"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true")
    opts = ap.parse_args(argv)
    table = extract_table()
    text = PY.read_text()
    new, count = re.subn(
        r"GAIN_TABLE = bytes\.fromhex\((?:\"[0-9a-f]*\"|\n(?:    \"[0-9a-f]+\"\n)*)\)",
        render(table), text)
    if count != 1:
        print("could not locate the GAIN_TABLE literal", file=sys.stderr)
        return 2
    if opts.check:
        if new != text:
            print("gm/sou2.py gain table is out of date", file=sys.stderr)
            return 1
        print(f"sou2 gain table ok ({len(table)} bytes)")
        return 0
    PY.write_text(new)
    print(f"wrote {len(table)} bytes into gm/sou2.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
