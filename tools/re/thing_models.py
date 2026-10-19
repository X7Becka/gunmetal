#!/usr/bin/env python3
"""Print the level's thing types and the model each one uses.

``THING_TYPES`` (``.sbt`` record 4) is a list of 936-byte records::

    char name[16]; double params[49]; u32 tail[132]

The model of a thing type is ``tail[3]`` -- byte offset **+0x1a4** -- an index
into the level's ``SOB_TABLE`` (record 20).  The value is written by the text
loader of ``thing\\<name>.soe``: the last field of the record line
(``"%s %lf %lf %lf %lf %lf %lf %s"``, ``0x4bbae0``) is the model *name*, and
``0x47c9b0`` (add/find SOB by name) turns it into the index, which the loader
stores at ``[936*i + 0x776f04]`` = ``[936*i + 0x776d60 + 0x1a4]`` (``0x47cd75``).
A spawned object carries the same index at ``obj+0x18``, which ``0x47c990``
turns into the model and ``0x4277cd`` assembles -- i.e. the chain is

    script entity -> object -> thing type (+0x1a4) -> SOB_TABLE -> SOBS/<name>.sob

Checks over the shipped data: every ``+0x1a4`` is a valid index (2896/2896) and
1036 point at the model *named like the type*; the rest name a shared model
(``bgchunk1`` -> ``BIGCHUNK``, ``deadtick``/``dedtick1`` -> ``TICKX``, ...).

Usage:  python3 thing_models.py [--game DIR] [--level STEM] [--json]
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from gm import sbt as sbtmod          # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MODEL_OFF = 0x1A4                     # tail[3]: SOB_TABLE index of the model


def models(sbt) -> dict:
    """``{thing type name: (index, model name or None)}`` of one level."""
    sob = [e.name for e in sbt.records_of(20)[0].payload.entries]
    out = {}
    for tt in sbt.records_of(4)[0].payload.entries:
        vals = struct.unpack("<%dI" % (len(tt.tail) // 4), tt.tail)
        rel = MODEL_OFF - 0x198
        idx = vals[rel // 4]
        out[tt.name] = (idx, sob[idx] if 0 <= idx < len(sob) else None)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--level", help="only this level stem (e.g. level2)")
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
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        data[stem] = models(sbtmod.parse_file(path))

    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
        return 0

    total = valid = named = 0
    alias = collections.Counter()
    for types in data.values():
        for name, (idx, model) in types.items():
            total += 1
            if model is None:
                continue
            valid += 1
            if model.upper() == name.upper():
                named += 1
            else:
                alias[(name, model)] += 1

    if args.level:
        print("thing-типы %s (модель = SOB_TABLE[+0x1a4]):" % args.level)
        for name in sorted(data[args.level]):
            idx, model = data[args.level][name]
            print("   %-14s +0x1a4=%-4d -> %s" % (name, idx, model or "??"))
    else:
        print("примеры (тип -> модель):")
        for (name, model), n in alias.most_common(8):
            print("   %-14s -> %-12s x%d" % (name, model, n))
    print("\nthing models: %d types in %d levels; valid SOB index %d/%d;"
          " name == type %d" % (total, len(data), valid, total, named))
    return 0 if valid == total else 1


if __name__ == "__main__":
    sys.exit(main())
