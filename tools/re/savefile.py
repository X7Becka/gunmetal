#!/usr/bin/env python3
"""Inspect a Gunmetal save (``SAVEGAME/saved<N>.sbt``).

A save is not a bespoke format: the ``.sbw`` is a normal level world (it parses
with :mod:`gm.sbw`, identical to the shipped one apart from the texture table),
and the ``.sbt`` is the level script with saved state.  The container starts
with::

    char magic[12]  "A SAVED GAME"
    u32  version    0x34 in the shipped save
    ...            header, level name at +0x54 ("LEVEL0")

The original ``<level>.sbt`` payload is embedded verbatim (with a growing byte
shift where state blocks are inserted), and a large state block is appended
after it.  This tool locates the source level, the script copy and the appended
state by matching the shipped level files, so it works without hard-coded
addresses.

Usage::

    python3 tools/re/savefile.py SAVEGAME/saved0.sbt [--game DIR] [--json]
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

MAGIC = b"A SAVED GAME"
VERSION_AT = 12
NAME_AT = 0x54
NAME_LEN = 16


def anchors(path: str) -> list[bytes]:
    data = open(path, "rb").read()
    out = []
    for off in range(0, max(1, len(data) - 256), 2048):
        chunk = data[off:off + 256]
        if chunk.count(0) < 200:        # skip zero runs, they match everywhere
            out.append(chunk)
    return out


def match_level(save: bytes, game: str) -> tuple[str, list[int]] | None:
    """Find the shipped ``.sbt`` whose body is embedded in *save*."""
    best = None
    for path in sorted(glob.glob(os.path.join(game, "*.sbt"))):
        hits = []
        for a in anchors(path):
            i = save.find(a)
            if i >= 0:
                hits.append(i)
        if len(hits) >= 3 and (best is None or len(hits) > len(best[1])):
            best = (os.path.basename(path), hits)
    return best


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("save")
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    data = open(args.save, "rb").read()
    if data[:len(MAGIC)] != MAGIC:
        print("not a save: %r" % data[:12], file=sys.stderr)
        return 1
    version = struct.unpack("<I", data[VERSION_AT:VERSION_AT + 4])[0]
    name = data[NAME_AT:NAME_AT + NAME_LEN].split(b"\0")[0].decode("latin1")

    src = match_level(data, args.game)
    info = {"file": os.path.basename(args.save), "bytes": len(data),
            "magic": MAGIC.decode(), "version": version, "level_name": name}
    if src:
        level, hits = src
        info["source_level"] = level
        info["script_hits"] = len(hits)
        # the first match gives the shift of the embedded script; the last
        # shipped anchor plus its length bounds the tail
        src_data = open(os.path.join(args.game, level), "rb").read()
        last_anchor = 0
        for off in range(0, max(1, len(src_data) - 256), 2048):
            chunk = src_data[off:off + 256]
            if chunk.count(0) < 200:
                i = data.find(chunk)
                if i >= 0:
                    last_anchor = max(last_anchor, i + 256)
        info["script_span"] = [hits[0], last_anchor]
        info["state_tail"] = [last_anchor, len(data) - last_anchor]

    if args.json:
        print(json.dumps(info, indent=2))
        return 0

    print("%s: %d bytes, container %r, version 0x%x"
          % (info["file"], info["bytes"], info["magic"], version))
    print("  level name in header: %r" % name)
    if src:
        print("  source level: %s (%d matching anchors)"
              % (info["source_level"], info["script_hits"]))
        print("  embedded script: 0x%x .. 0x%x" % tuple(info["script_span"]))
        print("  appended state:  0x%x (%d bytes)"
              % (info["state_tail"][0], info["state_tail"][1]))
    else:
        print("  source level: not matched against the installed .sbt files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
