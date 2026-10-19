#!/usr/bin/env python3
"""Doors, switches and lifts of the levels (``ADD_DOOR``/``ADD_SWITCH``/``ADD_LIFT``).

* ``ADD_DOOR`` (op 184, sig ``LEEEEEAAALLLLLLLDDDDDDDDDVNVNVN``) -- handler
  ``0x459b5b`` -> ``0x44e250`` stores the operands **permuted** into the door
  array ``0x8381a0`` (stride ``0xB8``, count ``0x5a9450``, at most 100).  The
  record therefore holds the flags at ``+0x68``, the five ``E`` ids at
  ``+0x6c..+0x7c`` and the seven ``L`` ints at ``+0x80..+0x9c``; ``GENERATE``
  finds a door by the id in ``0x44f690`` (it compares ``+0x6c/+0x70/+0x74``)
  and toggles the state words at ``+0x80`` and ``+0x8c`` (two of those ``L``s).
* ``ADD_SWITCH`` (op 183, sig ``LELLLLVN``) -- ``0x45b6ff`` -> ``0x44e5f0``
  copies the eight operands **verbatim** into ``0x5a4af0`` (stride ``0x20``,
  count ``0x5a945c``, at most 512).  Consumer ``0x44f960`` tests operand 1
  against ``0x101``, takes **operand 2 (the ``E``) as the id** and calls the
  ``GENERATE`` wrapper ``0x479960`` with operands 4/5/6 as the source context --
  those are the trigger's position (they fall inside the level's core geometry
  box for 86.9% of the shipped switches).
* ``ADD_LIFT`` (structural op 188): ``0x94`` bytes + ``n`` stops of ``0xc``
  (text loader layout); the fields are not decoded (45 lifts in the game).

The ids of all three live in the same space as the entity-table ids (the
scripts, doors and switches address one another by them): 88.9% of the distinct
switch ids and 86.1% of the distinct ``GENERATE`` ids are entity-table ids, and
97.6% of the ``GENERATE`` ids outside the table are door/switch ids.

Usage:  python3 doors.py [--game DIR] [--level STEM] [--frame] [--json]
"""
import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from gm import scene as scenemod          # noqa: E402
from gm import sbt as sbtmod              # noqa: E402
from gm import sbw as sbwmod              # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"


def entities_ids(sc) -> set:
    return {e[0] & 0xFFFFFFFF for e in sc.entities}


def core_box(level):
    """Bounding box of the level's geometry without the sky objects."""
    vs = [v for s in level.sectors for o in s.objects if o.vertices
          and max(max(abs(x[i]) for x in o.vertices) for i in range(3)) < 4000
          for v in o.vertices]
    if not vs:
        return None
    return [(min(v[i] for v in vs), max(v[i] for v in vs)) for i in range(3)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--level", help="only this level stem (e.g. dm1)")
    ap.add_argument("--frame", action="store_true",
                    help="also test the switch positions against the level box")
    ap.add_argument("--json", action="store_true", help="dump everything as JSON")
    args = ap.parse_args(argv)

    files = sorted(glob.glob(os.path.join(args.game, "*.sbt")))
    if args.level:
        files = [p for p in files
                 if os.path.splitext(os.path.basename(p))[0] == args.level]
    if not files:
        print("no matching .sbt files in %s" % args.game, file=sys.stderr)
        return 1

    data = {}
    totals = {"doors": 0, "switches": 0, "lifts": 0, "sliding": 0, "linked": 0,
              "sw_id": 0, "sw_id_ok": 0, "pos": 0, "pos_ok": 0}
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        recs = sbtmod.parse_file(path).records_of(11)
        sc = scenemod.extract(recs[0].payload)
        ids = entities_ids(sc)
        d = {"doors": [{"flags": x.flags, "ids": list(x.ids),
                        "state": list(x.state),
                        "angles": list(x.angles), "params": list(x.params)}
                       for x in sc.doors],
             "switches": [{"kind": x.kind, "id": x.id, "radius": x.radius,
                           "pos": list(x.pos), "extra": list(x.extra)}
                          for x in sc.switches],
             "lifts": [{"stops": len(x.stops), "head": len(x.head)}
                       for x in sc.lifts]}
        totals["doors"] += len(sc.doors)
        totals["sliding"] += sum(1 for x in sc.doors if x.moving)
        totals["linked"] += sum(1 for x in sc.doors if len(x.ids) >= 4 and x.ids[3])
        totals["switches"] += len(sc.switches)
        totals["lifts"] += len(sc.lifts)
        for s in sc.switches:
            totals["sw_id"] += 1
            if (s.id & 0xFFFFFFFF) in ids:
                totals["sw_id_ok"] += 1
        if args.frame and sc.switches:
            box = core_box(sbwmod.parse_file(
                os.path.join(args.game, stem + ".sbw")))
            if box:
                for s in sc.switches:
                    totals["pos"] += 1
                    # script coordinates are engine world; the box is the
                    # .sbw file frame (world / 1.875) -- see gm.scene.SCALE
                    if all(box[i][0] <= s.pos[i] * scenemod.SCALE <= box[i][1]
                           for i in range(3)):
                        totals["pos_ok"] += 1
        data[stem] = d

    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
        return 0

    if args.level:
        for stem in sorted(data):
            d = data[stem]
            for x in d["doors"]:
                print("door   flags=%d ids=%s state=%s angles=%s"
                      % (x["flags"], x["ids"], x["state"], x["angles"]))
            for x in d["switches"]:
                print("switch id=%-12d kind=%d pos=%s radius=%d"
                      % (x["id"], x["kind"], x["pos"], x["radius"]))
            for x in d["lifts"]:
                print("lift   stops=%d head=%dB" % (x["stops"], x["head"]))
    print("\ndoors/switches/lifts: %d doors (%d sliding, %d linked), "
          "%d switches, %d lifts"
          % (totals["doors"], totals["sliding"], totals["linked"],
             totals["switches"], totals["lifts"]))
    print("  switch ids in the level's entity table: %d/%d"
          % (totals["sw_id_ok"], totals["sw_id"]))
    if args.frame:
        print("  switch positions inside the core geometry box: %d/%d"
              % (totals["pos_ok"], totals["pos"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
