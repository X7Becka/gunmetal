#!/usr/bin/env python3
"""Print the Gunmetal campaign / deathmatch transition map.

Every level ends with ``NEW_LEVEL "stem"`` inside its ``SCRIPT`` block, guarded
by conditions such as ``DEATHMATCH_CLASSIC_GAME`` (all exits are taken from the
decoded script; ``docs/FORMATS.md`` 15).  Canonical single player goes through
the hub maps (``level00``/``level000``/``level0a`` = HEADQUARTERS,
``level0b`` = COMMAND POST), which is where the next mission is picked, while
deathmatch classic chains straight to the next shooting map::

    dm1 -> dm2 -> dm3 -> dm4 -> level1 -> level3 -> level4 -> level5 -> ...

The list of levels the "SELECT LEVEL FOR NEW GAME" menu offers is *not* a
hardcoded table: ``0x48b170`` scans the game directory for ``*.S?W``
(``0x48b1c0``, pattern ``"%s\\*.%s"``), sorts the names (``0x49ff70`` with the
comparator ``0x48b350`` that puts ``LEVEL*`` first and then orders by length)
and looks each stem up in ``LVLNAMES.SLU`` (``0x48a4c0``) for the display name.

Usage:  python3 campaign.py [--game DIR] [--json]
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
FIELDS = ("SET_LEVEL_NAME", "SET_LEVEL_SECRETS", "SET_LEVEL_BEST_TIME",
          "SET_LEVEL_FOG_DISTANCE", "DEFINE_LEVEL_OBJECTIVES", "NEW_LEVEL",
          "NEXT_LEVEL")


def _walk(insns, conds=()):
    """Yield ``(insn, conditions)``, recursing into ``IF``/``ELSE`` bodies."""
    for ins in insns:
        yield ins, conds
        body = getattr(ins, "body", None)
        if not isinstance(body, list):
            continue
        if ins.name == "IF":
            name = ins.args[0][0] if ins.args else "?"
            yield from _walk(body, conds + (name,))
        elif ins.name == "ELSE":
            yield from _walk(body, conds + ("else",))
        else:
            yield from _walk(body, conds)


def _args(ins):
    return [a[1] if isinstance(a, tuple) else a for a in (ins.args or [])]


def level(stem, path, stems):
    """Script facts of one level: name/objectives/secrets and exits."""
    info = {"stem": stem, "name": None, "objectives": None, "secrets": None,
            "best_time": None, "fog_distance": None, "exits": []}
    sbt = sbtmod.parse_file(path)
    recs = sbt.records_of(11)
    if not recs:
        return info
    for ins, conds in _walk(sbtvm.decode(recs[0].payload.code)[0]):
        if ins.name not in FIELDS:
            continue
        args = _args(ins)
        if ins.name == "SET_LEVEL_NAME":
            info["name"] = info["name"] or args[0]
        elif ins.name == "DEFINE_LEVEL_OBJECTIVES":
            info["objectives"] = args
        elif ins.name == "SET_LEVEL_SECRETS":
            info["secrets"] = args[0]
        elif ins.name == "SET_LEVEL_BEST_TIME":
            info["best_time"] = args[0]
        elif ins.name == "SET_LEVEL_FOG_DISTANCE":
            info["fog_distance"] = args[0]
        elif ins.name == "NEW_LEVEL":
            target = stems.get(str(args[0]).lower(), str(args[0]))
            info["exits"].append({"target": target, "conditions": list(conds)})
        elif ins.name == "NEXT_LEVEL":
            info["exits"].append({"target": None, "conditions": list(conds)})
    return info


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--json", action="store_true", help="dump the map as JSON")
    args = ap.parse_args(argv)

    files = sorted(glob.glob(os.path.join(args.game, "*.sbt")))
    if not files:
        print("no .sbt files in %s" % args.game, file=sys.stderr)
        return 1
    stems = {os.path.splitext(os.path.basename(p))[0].lower():
             os.path.splitext(os.path.basename(p))[0] for p in files}
    map_ = {os.path.splitext(os.path.basename(p))[0]: level(
        os.path.splitext(os.path.basename(p))[0], p, stems) for p in files}

    if args.json:
        print(json.dumps(map_, indent=2, sort_keys=True))
        return 0

    def order(stem):
        return (0 if stem.startswith("level") else 1 if stem.startswith("dm")
                else 2, stem)

    print("%-9s %-27s %-5s %-5s %s" % ("level", "name", "secr", "best", "exits"))
    for stem in sorted(map_, key=order):
        i = map_[stem]
        exits = "; ".join(
            "%s%s" % (e["target"] or "NEXT_LEVEL",
                      " [%s]" % ",".join(e["conditions"]) if e["conditions"] else "")
            for e in i["exits"]) or "-"
        print("%-9s %-27s %-5s %-5s %s" % (
            stem, i["name"] or "?", i["secrets"] if i["secrets"] is not None else "-",
            i["best_time"] if i["best_time"] is not None else "-", exits))
    return 0


if __name__ == "__main__":
    sys.exit(main())
