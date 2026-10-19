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

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import scene as scenemod  # noqa: E402

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


def find_block(tail: bytes, count: int, stride: int, key_off: int,
               keys: list) -> int:
    """Locate ``u32 count`` + *count* records of *stride* whose key matches."""
    if not count or count * stride + 4 > len(tail):
        return -1
    want = struct.pack("<I", count)
    start = 0
    while True:
        i = tail.find(want, start)
        if i < 0:
            return -1
        start = i + 1
        ok = True
        for k in range(count):
            off = i + 4 + k * stride + key_off
            if struct.unpack("<i", tail[off:off + 4])[0] != keys[k]:
                ok = False
                break
        if ok:
            return i


def blocks(tail: bytes, game: str, level: str) -> dict:
    """Door and switch arrays are stored as their raw runtime records."""
    sc = scenemod.load(os.path.join(game, level))
    doors, switches = list(sc.doors), list(sc.switches)
    out = {"level_doors": len(doors), "level_switches": len(switches)}
    # doors: flags +0x68, ids +0x6c..+0x7c, state +0x80, phase +0x8c
    off = find_block(tail, len(doors), 0xB8, 0x6c,
                     [tuple(d.ids)[0] for d in doors]) if doors else -1
    if off >= 0:
        recs = []
        for k, d in enumerate(doors):
            r = tail[off + 4 + k * 0xB8: off + 4 + (k + 1) * 0xB8]
            ids = struct.unpack("<5i", r[0x6c:0x80])
            if ids != tuple(d.ids):
                continue
            recs.append({"index": k, "flags": struct.unpack("<i", r[0x68:0x6c])[0],
                         "ids": list(ids),
                         "state": struct.unpack("<i", r[0x80:0x84])[0],
                         "phase": struct.unpack("<H", r[0x8c:0x8e])[0]})
        out["doors"] = {"offset": off, "records": recs}
    # switches: kind, id, radius, x, y, z, brake, unk (verbatim operands)
    off = find_block(tail, len(switches), 0x20, 4,
                     [s.id for s in switches]) if switches else -1
    if off >= 0:
        recs = []
        for k, s in enumerate(switches):
            r = tail[off + 4 + k * 0x20: off + 4 + (k + 1) * 0x20]
            vals = struct.unpack("<8i", r)
            recs.append({"index": k, "kind": vals[0], "id": vals[1],
                         "radius": vals[2], "pos": list(vals[3:6])})
        out["switches"] = {"offset": off, "records": recs}
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("save")
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    data = open(args.save, "rb").read()
    # the first 12 bytes hold the save's display name: "A SAVED GAME" for a
    # manually named save, otherwise whatever the slot name is (e.g. "2").
    title = data[:12].split(b"\0")[0].decode("latin1")
    if data[:len(MAGIC)] == MAGIC:
        version = struct.unpack("<I", data[VERSION_AT:VERSION_AT + 4])[0]
    else:
        version = struct.unpack("<I", data[:4])[0]
    name = data[NAME_AT:NAME_AT + NAME_LEN].split(b"\0")[0].decode("latin1")
    if not name.startswith("LEVEL") and not name[:1].isalpha():
        print("not a save: %r" % data[:16], file=sys.stderr)
        return 1

    src = match_level(data, args.game)
    info = {"file": os.path.basename(args.save), "bytes": len(data),
            "title": title, "version": version, "level_name": name}
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

    if src:
        info.update(blocks(data[info["state_tail"][0]:], args.game,
                           info["source_level"]))

    if args.json:
        print(json.dumps(info, indent=2))
        return 0

    print("%s: %d bytes, save name %r, version 0x%x"
          % (info["file"], info["bytes"], title, version))
    print("  level name in header (0x54): %r" % name)
    if src:
        print("  source level: %s (%d matching anchors)"
              % (info["source_level"], info["script_hits"]))
        print("  embedded script: 0x%x .. 0x%x" % tuple(info["script_span"]))
        print("  appended state:  0x%x (%d bytes)"
              % (info["state_tail"][0], info["state_tail"][1]))
    else:
        print("  source level: not matched against the installed .sbt files")
        return 0

    d = info.get("doors")
    if d:
        states = {}
        for r in d["records"]:
            states[r["state"]] = states.get(r["state"], 0) + 1
        print("  doors block at +0x%x: %d records, states %s"
              % (d["offset"], len(d["records"]), states))
        for r in d["records"]:
            if r["state"]:
                print("    door %2d ids=%s state=%+d phase=%d"
                      % (r["index"], r["ids"], r["state"], r["phase"]))
    s = info.get("switches")
    if s:
        print("  switches block at +0x%x: %d records"
              % (s["offset"], len(s["records"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
