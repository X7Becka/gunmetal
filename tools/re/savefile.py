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
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import scene as scenemod  # noqa: E402

MAGIC = b"A SAVED GAME"
VERSION_AT = 12
NAME_AT = 0x54
NAME_LEN = 16

# --- player block -----------------------------------------------------------
# The tail is a stream of ``[u8 id][payload]`` blocks; the player status block
# (id 9, see ``0x452020``) is ``u8 9``, ``u8 present``, then the raw 0x3510
# runtime struct, then 16 bytes per attached object.  Gauges are stored in
# hundredths of a point (10000 == 100.00), proven against ``0x44408a`` (armor
# max/current ratio) and the damage loop at ``0x443c7d`` (4 shields).
PLAYER_ID = 9
PLAYER_STRUCT = 0x3510
ARMOR_MAX_AT = 0x3244
ARMOR_AT = 0x3248
SHIELD_MAX_AT = 0x324c
SHIELDS_AT = 0x3250
RESOURCE_AT = 0x32c4
SHIELD_NAMES = ("front", "back", "left", "right")
GAUGE = 100.0

# weapons: the selected slot (+0x3294), the level's weapon bitmask (+0x3298)
# and one u32 ammo counter per slot (+0x329c), indexed the same way as the
# runtime weapon table (``[slot*4 + 0x81c2dc]`` at ``0x4455ad``).
WEAPON_SLOT_AT = 0x3294
WEAPON_MASK_AT = 0x3298
AMMO_AT = 0x329c
AMMO_SLOTS = 12


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


def find_player(tail: bytes) -> int:
    """Locate the player status block (marker 9 + present + 0x3510 struct)."""
    hits = []
    start = 0
    while True:
        i = tail.find(bytes((PLAYER_ID, 1)), start)
        if i < 0 or i + 2 + 0x3260 > len(tail):
            break
        start = i + 1
        st = tail[i + 2:i + 2 + PLAYER_STRUCT]
        amax, acur, smax = struct.unpack("<3i", st[ARMOR_MAX_AT:SHIELDS_AT])
        shields = struct.unpack("<4i", st[SHIELDS_AT:SHIELDS_AT + 16])
        if not 0 < amax <= 1000000 or not 0 <= acur <= amax:
            continue
        if not 0 < smax <= 1000000:
            continue
        if not all(0 <= v <= smax for v in shields):
            continue
        hits.append(i)
    return hits[-1] if hits else -1


def player(tail: bytes) -> dict:
    """Player gauges, selected weapon and the per-slot ammo counters."""
    off = find_player(tail)
    if off < 0:
        return {}
    st = tail[off + 2:off + 2 + PLAYER_STRUCT]
    amax, acur, smax = struct.unpack("<3i", st[ARMOR_MAX_AT:SHIELDS_AT])
    shields = struct.unpack("<4i", st[SHIELDS_AT:SHIELDS_AT + 16])
    slot = struct.unpack("<i", st[WEAPON_SLOT_AT:WEAPON_SLOT_AT + 4])[0]
    mask = struct.unpack("<I", st[WEAPON_MASK_AT:WEAPON_MASK_AT + 4])[0]
    ammo = struct.unpack("<%dI" % AMMO_SLOTS, st[AMMO_AT:AMMO_AT + 4 * AMMO_SLOTS])
    return {"offset": off,
            "armor": acur / GAUGE, "armor_max": amax / GAUGE,
            "shield_max": smax / GAUGE,
            "shields": {n: v / GAUGE for n, v in zip(SHIELD_NAMES, shields)},
            "weapon_slot": slot, "weapon_mask": mask,
            "ammo": {i: v for i, v in enumerate(ammo)}}


def weapons(level_path: str) -> list[str]:
    """Weapon names in table order (the ``WICON`` entries of the level's .sbt).

    The runtime weapon table (``0x80fa3c``, stride ``0x2a8``) is filled from the
    level's ``DEFINE_WEAPONS`` block, which stores the name right before the
    icon tag (``clickblt``/``WICON<n>``).  Ammo slots follow the same order,
    which is what makes slot 0 the machine gun and slot 1 the cannon.
    """
    try:
        data = open(level_path, "rb").read()
    except OSError:
        return []
    names: list[str] = []
    for m in re.finditer(rb"WICON\d+\x00", data):
        pre = data[max(0, m.start() - 96):m.start()]
        parts = [p for p in pre.split(b"\x00")
                 if p and all(32 <= c < 127 for c in p)]
        if len(parts) < 2:
            continue
        name = parts[-2].decode("latin1").strip()
        if name and name not in names:
            names.append(name)
    return names



def weapon_table(level_path: str) -> list[dict]:
    """Per-slot weapon name, ammo type and stock limit from the level script.

    ``DEFINE_WEAPONS`` field ``L3`` is the ammo type index and ``DEFINE_AMMO``
    field ``L0`` is the stock limit (the shop clamps to it, ``0x47b642``), so
    the pair gives the per-slot maximum that the HUD shows next to the count.
    """
    try:
        from gm import sbt as sbtmod, sbtvm
        s = sbtmod.parse_file(level_path)
    except Exception:
        return []
    code = next((r.payload.code for r in s.records if r.name == "SCRIPT"), None)
    if not code:
        return []
    try:
        insns, _end = sbtvm.decode(code)
    except Exception:
        return []
    ammo: list = []
    weapons: list = []
    for ins in insns:                    # top-level: the DEATHMATCH copy is nested
        if ins.name == "DEFINE_AMMO" and ins.data:
            ammo = ins.data
        elif ins.name == "DEFINE_WEAPONS" and ins.data:
            weapons = ins.data
    out = []
    for strs, vals in weapons:
        idx = vals[2] if len(vals) > 2 else -1
        cap = None
        kind = ""
        if 0 <= idx < len(ammo):
            kind = ammo[idx][0][0] if ammo[idx][0] else ""
            cap = ammo[idx][1][0] if ammo[idx][1] else None
        out.append({"name": strs[0] if strs else "", "ammo_type": kind,
                    "max": cap})
    return out


def blocks(tail: bytes, game: str, level: str) -> dict:
    """Door and switch arrays are stored as their raw runtime records."""
    sc = scenemod.load(os.path.join(game, level))
    doors, switches = list(sc.doors), list(sc.switches)
    out = {"level_doors": len(doors), "level_switches": len(switches)}
    p = player(tail)
    if p:
        level_path = os.path.join(game, level)
        table = weapon_table(level_path)
        if table:
            p["names"] = [w["name"] for w in table]
            p["ammo_max"] = [w["max"] for w in table]
            p["ammo_type"] = [w["ammo_type"] for w in table]
        else:
            p["names"] = weapons(level_path)
        out["player"] = p
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


def inspect(path: str, game: str) -> dict:
    """Parse a save: header, embedded level and decoded state blocks."""
    data = open(path, "rb").read()
    # the first 12 bytes hold the save's display name: "A SAVED GAME" for a
    # manually named save, otherwise whatever the slot name is (e.g. "2").
    title = data[:12].split(b"\0")[0].decode("latin1")
    if data[:len(MAGIC)] == MAGIC:
        version = struct.unpack("<I", data[VERSION_AT:VERSION_AT + 4])[0]
    else:
        version = struct.unpack("<I", data[:4])[0]
    name = data[NAME_AT:NAME_AT + NAME_LEN].split(b"\0")[0].decode("latin1")
    if not name.startswith("LEVEL") and not name[:1].isalpha():
        raise ValueError("not a save: %r" % data[:16])
    src = match_level(data, game)
    info = {"file": os.path.basename(path), "bytes": len(data),
            "title": title, "version": version, "level_name": name}
    if not src:
        return info
    level, hits = src
    info["source_level"] = level
    info["script_hits"] = len(hits)
    # the first match gives the shift of the embedded script; the last shipped
    # anchor plus its length bounds the tail
    src_data = open(os.path.join(game, level), "rb").read()
    last_anchor = 0
    for off in range(0, max(1, len(src_data) - 256), 2048):
        chunk = src_data[off:off + 256]
        if chunk.count(0) < 200:
            i = data.find(chunk)
            if i >= 0:
                last_anchor = max(last_anchor, i + 256)
    info["script_span"] = [hits[0], last_anchor]
    info["state_tail"] = [last_anchor, len(data) - last_anchor]
    info.update(blocks(data[last_anchor:], game, level))
    return info


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("save")
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        info = inspect(args.save, args.game)
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1
    title, version, name = info["title"], info["version"], info["level_name"]
    src = info.get("source_level")

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
    p = info.get("player")
    if p:
        print("  player block at +0x%x: armor %.2f/%.2f"
              % (p["offset"], p["armor"], p["armor_max"]))
        print("    shields (max %.2f): %s"
              % (p["shield_max"],
                 " ".join("%s=%.2f" % (n, p["shields"][n]) for n in SHIELD_NAMES)))
        names = p.get("names") or []
        sel = p["weapon_slot"]
        print("    weapons: mask 0x%x, selected %s"
              % (p["weapon_mask"],
                 "%d (%s)" % (sel, names[sel]) if 0 <= sel < len(names) else sel))
        for i in sorted(p["ammo"]):
            if not p["ammo"][i]:
                continue
            cap = (p.get("ammo_max") or [None] * 99)
            cap = cap[i] if i < len(cap) else None
            print("      ammo %-16s %d%s"
                  % (names[i] if i < len(names) else "slot %d" % i,
                     p["ammo"][i], " / %d" % cap if cap else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
