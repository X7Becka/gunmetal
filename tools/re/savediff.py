#!/usr/bin/env python3
"""Byte-diff two Gunmetal saves of the **same** level.

A save is a level script with saved state (see ``tools/re/savefile.py``): header
``0x8e8`` bytes with the level name at ``+0x54``, the level's own ``.sbt`` body
embedded verbatim, and a state tail appended after it.  Two saves of the same
level share the script (it is a byte copy), so a diff shows exactly the state.

Usage::

    python3 tools/re/savediff.py SAVEGAME/saved1.sbt SAVEGAME/saved2.sbt

The tool refuses to compare saves of different levels (the script differs in
that case, which would drown the state).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import savefile  # noqa: E402


def load(path: str, game: str):
    data = open(path, "rb").read()
    name = data[0x54:0x64].split(b"\0")[0].decode("latin1")
    src = savefile.match_level(data, game)
    if not src:
        raise SystemExit("%s: source level not found" % path)
    level, hits = src
    src_data = open(os.path.join(game, level), "rb").read()
    end = 0
    for off in range(0, max(1, len(src_data) - 256), 2048):
        chunk = src_data[off:off + 256]
        if chunk.count(0) < 200:
            i = data.find(chunk)
            if i >= 0:
                end = max(end, i + (len(src_data) - off))
    return {"path": path, "data": data, "level": level, "name": name,
            "tail": end}


def ranges(a: bytes, b: bytes, limit: int = 40):
    """Group differing byte offsets into ``(start, end)`` runs."""
    n = min(len(a), len(b))
    out = []
    start = None
    for i in range(n):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            out.append((start, i))
            start = None
            if len(out) >= limit:
                return out, True
    if start is not None:
        out.append((start, n))
    return out, False


def describe(data: bytes, start: int, end: int) -> str:
    chunk = data[start:end][:16]
    ints = []
    for i in range(0, min(len(chunk), 12), 4):
        ints.append("%d" % struct.unpack("<i", chunk[i:i + 4].ljust(4, b"\0"))[0])
    return " ".join(ints)


def diff(first: str, second: str, game: str, limit: int = 40,
         phases: bool = False) -> int:
    a = load(first, game)
    b = load(second, game)
    print("%s: %s (%s, state tail at 0x%x, %d bytes)"
          % (os.path.basename(a["path"]), a["name"], a["level"],
             a["tail"], len(a["data"]) - a["tail"]))
    print("%s: %s (%s, state tail at 0x%x, %d bytes)"
          % (os.path.basename(b["path"]), b["name"], b["level"],
             b["tail"], len(b["data"]) - b["tail"]))
    if a["level"] != b["level"]:
        print("\ndifferent levels (%s vs %s): the embedded scripts differ, so a "
              "state diff is not meaningful" % (a["level"], b["level"]))
        return 2
    if len(a["data"]) == len(b["data"]):
        whole = True
    else:
        whole = False

    # decoded state: door records are raw runtime records (see savefile.py)
    da = savefile.blocks(a["data"][a["tail"]:], game, a["level"])
    db = savefile.blocks(b["data"][b["tail"]:], game, b["level"])
    if "doors" in da and "doors" in db:
        ra = {r["index"]: r for r in da["doors"]["records"]}
        rb = {r["index"]: r for r in db["doors"]["records"]}
        flipped = [i for i in sorted(ra) if i in rb
                   and ra[i]["state"] != rb[i]["state"]]
        print("\nchanged doors (state): %d of %d" % (len(flipped), len(ra)))
        for i in flipped:
            print("  door %2d ids=%s  state %+d -> %+d"
                  % (i, ra[i]["ids"], ra[i]["state"], rb[i]["state"]))
        if phases:
            moved = [i for i in sorted(ra) if i in rb and i not in flipped
                     and ra[i]["phase"] != rb[i]["phase"]]
            print("phase-only changes: %d" % len(moved))
            for i in moved:
                print("  door %2d ids=%s  phase %d -> %d"
                      % (i, ra[i]["ids"], ra[i]["phase"], rb[i]["phase"]))
    if "switches" in da and "switches" in db:
        sa = {r["index"]: r for r in da["switches"]["records"]}
        sb = {r["index"]: r for r in db["switches"]["records"]}
        same = all(sa[i]["id"] == sb[i]["id"] and sa[i]["pos"] == sb[i]["pos"]
                   for i in sa if i in sb)
        print("switches: %d records, unchanged: %s" % (len(sa), same))
    pa, pb = da.get("player"), db.get("player")
    if pa and pb:
        print("player (block at +0x%x -> +0x%x): armor %.2f -> %.2f (max %.2f)"
              % (pa["offset"], pb["offset"], pa["armor"], pb["armor"],
                 pb["armor_max"]))
        names = pa.get("names") or []
        for i in sorted(pa["ammo"]):
            va, vb = pa["ammo"][i], pb["ammo"].get(i, 0)
            if va != vb:
                print("  ammo %-16s %6d -> %6d  (%+d)"
                      % (names[i] if i < len(names) else "slot %d" % i,
                         va, vb, vb - va))
        if pa["weapon_slot"] != pb["weapon_slot"]:
            print("  selected weapon %d -> %d" % (pa["weapon_slot"], pb["weapon_slot"]))
        for n in savefile.SHIELD_NAMES:
            va, vb = pa["shields"][n], pb["shields"][n]
            print("  shield %-5s %6.2f -> %6.2f%s"
                  % (n, va, vb, "" if va == vb else "  (%+.2f)" % (vb - va)))

    for tag, start in (("header", 0), ("state", a["tail"])):
        stop = 0x8e8 if tag == "header" else min(len(a["data"]), len(b["data"]))
        aa, bb = a["data"][start:stop], b["data"][start:stop]
        runs, truncated = ranges(aa, bb, limit)
        print("\n%s 0x%x..0x%x: %d differing run(s)%s"
              % (tag, start, stop, len(runs), " (truncated)" if truncated else ""))
        for lo, hi in runs[:12]:
            print("  %+8d..%-8d (%3d B)  A=%-28s B=%s"
                  % (lo, hi, hi - lo, describe(aa, lo, hi), describe(bb, lo, hi)))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("first")
    ap.add_argument("second")
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--phases", action="store_true",
                    help="also list doors whose animation phase changed")
    args = ap.parse_args()
    return diff(args.first, args.second, args.game, args.limit, args.phases)


if __name__ == "__main__":
    raise SystemExit(main())
