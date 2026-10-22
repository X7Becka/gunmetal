#!/usr/bin/env python3
"""Top-level structure of Gunmetal saves (``SAVEGAME\\saved<N>.*``).

A save is a *level pair*: the game formats ``savegame\\saved%d.sbt`` and
``savegame\\saved%d.sbw`` (strings ``0x4ab460`` / ``0x4ab478``) and writes the
current level there, followed by the runtime state of every subsystem.

The state goes through one dispatcher (``0x4550xx``): a system id in ``1..255``
is looked up in the byte table ``0x455658`` and then jumped through the pointer
table ``0x455604``.  Each branch saves (or, on the other half, loads) one block
through its own function; the blocks are ``u32 count`` + fixed-stride records
(doors ``0xB8``, switches ``0x20`` -- the loaders ``0x44e4f0`` / ``0x44f000``
implement exactly that, with the error strings listed below).

Usage::

    python3 tools/re/savegame.py [--exe PATH] [--json]
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import exelib  # noqa: E402

JUMP_TABLE = 0x455604          # dword pointers
INDEX_TABLE = 0x455658         # 255 bytes -> index into JUMP_TABLE
DISPATCHER = 0x455150          # cmp/jmp prologue (documented, not required)

#: handler address -> subsystem, when the code or its strings say so
NAMES = {
    0x44f000: "switches",        # "Error reading switch count."
    0x44e540: "doors",           # "Error reading door count."
    0x4564b0: "sprites",         # "Error reading sprites count."
    0x44ec20: "lifts",           # "Error reading lift count."
    0x451c20: "player status",   # "playerstatus_loadbin() ..."
    0x46fd90: "player sprite",   # "playerstatus_loadbin() unable to attached sprite info."
    0x40b670: "sound table",     # "Error loading sound sample table info."
    0x47e000: "world objects",   # walks 0x5670d8, the sector->object table
    0x462a80: "script state",    # touches 0x4b6784, the compiled .sbt code
    0x463680: "handle table",    # 0x5b1df8 / 0x5e3de0 / 0x617fb8
    0x463900: "handle table 2",  # same tables, the second half
}

#: the two file-name patterns the game formats for a save slot
PATTERNS = ((0x4AB460, "savegame\\saved%d.sbt"),
            (0x4AB478, "savegame\\saved%d.sbw"))


def _first_call(data, va, span=48):
    """First ``call rel32`` target after *va* (the branch's handler)."""
    off = exelib.va2off(va)
    for i in range(span):
        if data[off + i] == 0xE8:
            rel = struct.unpack("<i", data[off + i + 1:off + i + 5])[0]
            tgt = va + i + 5 + rel
            if 0x401000 <= tgt < 0x4a0000:
                return tgt
    return None


def table(data):
    """Decode ``(id -> handler)`` from the dispatcher's two tables."""
    off = exelib.va2off(JUMP_TABLE)
    ptrs = struct.unpack("<32I", data[off:off + 32 * 4])
    idx_off = exelib.va2off(INDEX_TABLE)
    ids = data[idx_off:idx_off + 255]
    out = {}
    for sid, group in enumerate(ids, start=1):
        if group < len(ptrs) and ptrs[group] >= 0x401000:
            out[sid] = _first_call(data, ptrs[group]) or ptrs[group]
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", default=None,
                    help="executable (default: the cache copy, else the install)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    exe = args.exe or exelib.cache_path("gundx.exe")
    if not os.path.exists(exe):
        exe = exelib.EXE          # cache not built yet
    data = open(exe, "rb").read()
    for va, want in PATTERNS:
        got = exelib.cstr(va, 64)
        if got != want:
            print("unexpected string at 0x%x: %r" % (va, got), file=sys.stderr)
            return 1

    systems = table(data)
    # ids that fall into the shared "unknown id" branch are not subsystems
    counts = {}
    for fn in systems.values():
        counts[fn] = counts.get(fn, 0) + 1
    default = [fn for fn, n in counts.items() if n > 8]
    if default:
        for sid in list(systems):
            if systems[sid] == default[0]:
                del systems[sid]
    handlers = sorted(set(systems.values()))
    rows = []
    for sid in sorted(systems):
        fn = systems[sid]
        rows.append({"id": sid, "handler": "0x%06x" % fn,
                     "name": NAMES.get(fn, "?")})
    if args.json:
        print(json.dumps({"format": "level pair (.sbt + .sbw)",
                          "patterns": [w for _, w in PATTERNS],
                          "systems": rows}, indent=2))
        return 0

    print("save is a level pair: %s + %s" % (PATTERNS[0][1], PATTERNS[1][1]))
    print("state dispatcher 0x%06x: %d ids, %d distinct handlers"
          % (DISPATCHER, len(systems), len(handlers)))
    for r in rows:
        print("  id %3d  %-14s %s" % (r["id"], r["name"], r["handler"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
