#!/usr/bin/env python3
"""Print how a Gunmetal level picks the player's craft (R.P.V.) model.

The chassis records (script command 172, ``DEFINE_PLAYER_TYPES``) carry, next
to the performance figures, the names of the hull resources, e.g.
``57 CHEVY | devsttr | devsttrx`` or ``LIGHT CHASSIS | tick | tickx``.  At run
time the craft model is resolved **by name** against the level's model table
(``.sbt`` record 20, ``SOB_TABLE``)::

    0x47c940(name)            name   -> SOB index   (or -1)
    0x47c910(index)           index  -> name
    0x47c990(index)           index  -> model data (loader 0x42b4e1:
                              "SOBS\\<name>.sob" or "Sobanim\\<name>.soo")
    0x452240(player, index)   swap the craft model: frees the old instance
                              (0x435a93), resolves 0x47c990, assembles it
                              (0x4277cd / 0x43b639) into [player+0x81c1b8],
                              records the index in [player+0x81c254] and hands
                              the three doubles [player+0x81bdb0/b8/c0] to
                              setobjectradius (0x41df24)
    0x5a95e8                  per-player craft *name* (0x10 bytes x 8)
    0x452430 / 0x452fd5       fill the name from the model index / resolve it
                              back when a level (re)starts or a player joins

Because the name has to resolve in every level, all 36 shipped ``SOB_TABLE``s
preload the eight chassis models; this tool prints them and the chassis
records side by side, plus the two invariants that follow:

* every level resolves the eight chassis names (36/36);
* ``CHEVY`` is in *no* level's table (0/36) -- ``Chevy.sob`` is unreachable,
  the bonus chassis reuses ``DEVSTTR`` (the heavy hull) and its textures.

Usage:  python3 chassis_models.py [--game DIR] [--json]
"""
import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm                  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
CHASSIS = ("DEVSTTR", "TICK", "SPARTAN", "STEALTH",
           "TICKX", "SPARTANX", "STEALTHX", "DEVSTTRX")


def _walk(insns):
    for ins in insns:
        yield ins
        if isinstance(getattr(ins, "body", None), list):
            yield from _walk(ins.body)


def level(path):
    """``(sob names, sounds, chassis records)`` of one level."""
    sbt = sbtmod.parse_file(path)
    sob = [e.name for e in sbt.records_of(20)[0].payload.entries]
    sounds = list(sbt.records_of(12)[0].payload.names)
    rows = []
    for ins in _walk(sbtvm.decode(sbt.records_of(11)[0].payload.code)[0]):
        if ins.name != "DEFINE_PLAYER_TYPES":
            continue
        for strings, vals in ins.data:
            rows.append({
                "name": strings[0], "hull": strings[1], "icon": strings[2],
                "mass_limit": vals[0], "armor": vals[2], "armor_cap": vals[3],
                "max_speed": vals[4], "op5": vals[5], "price": vals[10],
                "sound": vals[11],
                "sound_name": (sounds[vals[11]] if 0 <= vals[11] < len(sounds)
                               else None if vals[11] == 0xFFFFFFFF else "?"),
            })
    return sob, rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--json", action="store_true", help="dump everything as JSON")
    args = ap.parse_args(argv)

    files = sorted(glob.glob(os.path.join(args.game, "*.sbt")))
    if not files:
        print("no .sbt files in %s" % args.game, file=sys.stderr)
        return 1

    data = {}
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        sob, rows = level(path)
        data[stem] = {"sob": sob, "chassis": rows}

    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
        return 0

    ok_levels = sum(1 for d in data.values()
                    if all(n in [s.upper() for s in d["sob"]] for n in CHASSIS))
    chevy_levels = sum(1 for d in data.values()
                       if "CHEVY" in [s.upper() for s in d["sob"]])

    print("модели шасси в SOB_TABLE уровня (индекс: имя):")
    for stem in sorted(data):
        sob = data[stem]["sob"]
        idx = {n: i for i, n in enumerate(s.upper() for s in sob)}
        parts = " ".join("%s=%s" % (n, idx.get(n, "-")) for n in CHASSIS)
        print("  %-9s %s  (%d записей)" % (stem, parts, len(sob)))

    print("\nзаписи DEFINE_PLAYER_TYPES (сводно по всем уровням):")
    seen = {}
    blocks = 0
    for stem in data:
        blocks += sum(1 for _ in data[stem]["chassis"])
        for r in data[stem]["chassis"]:
            key = (r["name"], r["hull"], r["op5"], r["sound_name"])
            seen.setdefault(key, 0)
            seen[key] += 1
    print("  %-15s %-9s %-5s %-9s %s" % ("шасси", "текстура", "op5", "звук", "блоков"))
    for (name, hull, op5, snd), n in sorted(seen.items()):
        print("  %-15s %-9s %-5s %-9s %d" % (name, hull, op5, snd or "-", n))

    print("\nchassis models: %d blocks in %d levels; 8 names in SOB_TABLE: %d/%d;"
          " CHEVY: %d/%d" % (blocks, len(data), ok_levels, len(data),
                             chevy_levels, len(data)))
    return 0 if ok_levels == len(data) else 1


if __name__ == "__main__":
    sys.exit(main())
