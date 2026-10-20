#!/usr/bin/env python3
"""Nested ``ADD_THING*`` audit: top-level vs recursive vs executed things.

``gm.scene.things()`` historically stops at the top level of each entity
sub-script (4 008 things over the 36 shipped levels).  The compiler also emits
``ADD_THING*`` inside ``IF``/``ELSE`` bodies; a recursive static walk finds
**7 563** sites, and the VM executor builds **7 617** things over the full
context matrix (a few sites run twice in loops).  This tool makes the three
numbers explicit, checks that no site is double-counted, and compares the 391
player/DM starts.

Usage::

    python3 nested_things.py --check                 # all 36 .sbt, one context
    python3 nested_things.py --check --contexts all  # 2 game modes x 4 types
    python3 nested_things.py --level dm1 --contexts all --json
    python3 nested_things.py --self-test             # no game files needed

``.sbt`` files live outside the repository; ``GUNMETAL_GAME`` or ``--game``
selects the installation (default ``/mnt/d/games/Gunmetal 3dfx``).  The game is
opened read-only.
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod            # noqa: E402
from gm import scene as scenemod       # noqa: E402
from gm import sbtvm                   # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: corpus expectations (docs/VM_EXEC.md 6.1/6.2)
EXPECT_TOP = 4008
EXPECT_RECURSIVE = 7563
EXPECT_EXECUTED_ALL = 7617
EXPECT_UNREACHED = 38
EXPECT_SPAWNS = 391
EXPECT_LEVELS = 36


# --------------------------------------------------------------------------
# contexts


def make_contexts(args):
    """The :class:`gm.scene.ThingContext` list selected by the CLI."""
    if args.contexts == "all":
        base = [scenemod.ThingContext(game_mode=gm, playertype=pt,
                                      seed=args.seed)
                for gm in (0, 1) for pt in (0, 1, 2, 3)]
    else:
        base = [scenemod.ThingContext(seed=args.seed)]
    over = {}
    if args.difficulty is not None:
        over["difficulty"] = args.difficulty
    if args.players is not None:
        over["players"] = args.players
    if args.source_player is not None:
        over["source_player"] = args.source_player
    if args.god_mode:
        over["god_mode"] = True
    if args.objectives:
        over["objectives_complete"] = True
    if args.random_unknown:
        over["random_unknown"] = True
    if args.follow_generate:
        over["follow_generate"] = True
    if args.max_time is not None:
        over["max_time"] = args.max_time
    contexts = [c if not over else _replace(c, over) for c in base]
    if args.reach:
        # the two states that reach the last 38 static sites (docs §4.1/4.2)
        contexts.append(scenemod.ThingContext(
            game_mode=0, difficulty=10, objectives_complete=True,
            random_unknown=True, seed=args.seed))
        contexts.append(scenemod.ThingContext(
            game_mode=1, source_player=0, random_unknown=True,
            seed=args.seed))
    return contexts


def _replace(ctx, over):
    from dataclasses import replace
    return replace(ctx, **over)


# --------------------------------------------------------------------------
# one level


def block_offsets(script):
    out, seen = [], set()
    for eid, _flags, off in scenemod.entities(script):
        if off not in seen and 0 <= off < len(script.code):
            seen.add(off)
            out.append((eid, off))
    return out


def analyse_level(path, contexts):
    """Static and executed counts for one level."""
    sbt_file = sbtmod.parse_file(path)
    recs = sbt_file.records_of(11)
    if not recs:
        raise SystemExit("%s: no SCRIPT record" % path)
    script = recs[0].payload
    static = scenemod.things(script, sbt_file)
    recursive = scenemod.things(script, sbt_file, recursive=True)
    sites = {t.site for t in recursive}
    path_map = {t.site: (t.path, t.depth) for t in recursive}

    things, spawns, reasons = {}, {}, collections.Counter()
    per_context = {}
    if contexts:
        for ctx in contexts:
            res = scenemod.execute_things(script, sbt_file, ctx,
                                          path_map=path_map)
            for r, n in res.reasons.items():
                reasons[r] += n
            before = len(things)
            for t in res.things:
                things.setdefault((t.site, t.occ), t)
            for sp in res.spawns:
                spawns.setdefault((getattr(sp, "site", None),
                                   getattr(sp, "occ", None)), sp)
            per_context[ctx.label()] = {
                "things": len(res.things),
                "paths": res.paths,
                "blocks": res.blocks,
                "spawns": len(res.spawns),
            }

    static_spawns = collections.Counter(
        (s.index, s.pos, s.angles, s.kind) for s in scenemod.extract(script).spawns)
    got_spawns = collections.Counter(
        (s.index, s.pos, s.angles, s.kind) for s in spawns.values())
    exec_things = list(things.values())
    exec_sites = {t.site for t in exec_things}
    top = sum(1 for t in exec_things if t.depth == 0)
    return {
        "level": os.path.basename(path)[:-4],
        "top_level": len(static),
        "recursive": len(recursive),
        "nested": len(recursive) - len(static),
        "static_sites": len(sites),
        "static_duplicates": len(recursive) - len(sites),
        "executed": len(exec_things),
        "executed_sites": len(exec_sites),
        "unreached": len(sites - exec_sites),
        "executed_top_level": top,
        "executed_nested": len(exec_things) - top,
        "executed_duplicates": sum(1 for t in exec_things if t.occ),
        "contexts": per_context,
        "spawns_static": sum(static_spawns.values()),
        "spawns_executed": len(spawns),
        "spawns_missing": sum((static_spawns - got_spawns).values()),
        "spawns_extra": sum((got_spawns - static_spawns).values()),
        "reasons": dict(reasons),
    }


def analyse(paths, contexts):
    rows = [analyse_level(p, contexts) for p in paths]
    totals = {
        "levels": len(rows),
        "top_level": sum(r["top_level"] for r in rows),
        "recursive": sum(r["recursive"] for r in rows),
        "nested": sum(r["nested"] for r in rows),
        "static_duplicates": sum(r["static_duplicates"] for r in rows),
        "executed": sum(r["executed"] for r in rows),
        "executed_sites": sum(r["executed_sites"] for r in rows),
        "unreached": sum(r["unreached"] for r in rows),
        "spawns_static": sum(r["spawns_static"] for r in rows),
        "spawns_executed": sum(r["spawns_executed"] for r in rows),
        "spawns_missing": sum(r["spawns_missing"] for r in rows),
        "spawns_extra": sum(r["spawns_extra"] for r in rows),
    }
    return rows, totals


# --------------------------------------------------------------------------
# expectations / report


def expectations(totals, contexts, full=True):
    checks = {
        "static duplicates == 0": totals["static_duplicates"] == 0,
        "spawns missing == 0": totals["spawns_missing"] == 0,
        "spawns extra == 0": totals["spawns_extra"] == 0,
    }
    if full:
        checks.update({
            "levels == %d" % EXPECT_LEVELS: totals["levels"] == EXPECT_LEVELS,
            "top-level == %d" % EXPECT_TOP: totals["top_level"] == EXPECT_TOP,
            "recursive == %d" % EXPECT_RECURSIVE:
                totals["recursive"] == EXPECT_RECURSIVE,
            "spawns executed == %d" % EXPECT_SPAWNS:
                totals["spawns_executed"] == EXPECT_SPAWNS,
        })
        if len(contexts) >= 8 and contexts and contexts[0].random_unknown:
            if len(contexts) > 8:
                checks["all static sites reached (--reach)"] = \
                    totals["unreached"] == 0
            else:
                checks["executed (all contexts, RANDOM forked) == %d"
                       % EXPECT_EXECUTED_ALL] = \
                    totals["executed"] == EXPECT_EXECUTED_ALL
                checks["unreached == %d" % EXPECT_UNREACHED] = \
                    totals["unreached"] == EXPECT_UNREACHED
    return checks


def print_report(rows, totals, checks, contexts):
    print("%-9s %5s %6s %6s %6s %6s %5s %6s %s"
          % ("level", "top", "nested", "static", "exec", "dups", "spawn",
             "unreach", "reasons"))
    for r in rows:
        reasons = r["reasons"]
        bad = {k: v for k, v in reasons.items()
               if k not in ("end", "return", "program-exit",
                            "level-transition")}
        tag = "ok" if not bad else " ".join("%s=%d" % kv for kv in
                                            sorted(bad.items()))
        print("%-9s %5d %6d %6d %6d %6d %3d/%-3d %6d %s"
              % (r["level"], r["top_level"], r["nested"], r["recursive"],
                 r["executed"], r["static_duplicates"],
                 r["spawns_executed"], r["spawns_static"], r["unreached"], tag))
    print("%-9s %5d %6d %6d %6d %6d %3d/%-3d %6d"
          % ("TOTAL", totals["top_level"], totals["nested"],
             totals["recursive"], totals["executed"],
             totals["static_duplicates"], totals["spawns_executed"],
             totals["spawns_static"], totals["unreached"]))
    print("\ncontexts: %s" % ", ".join(c.label() for c in contexts))
    print("executed/static: %d/%d things (%+d, nested %+d); spawns %d/%d"
          % (totals["executed"], totals["recursive"],
             totals["executed"] - totals["recursive"],
             totals["executed"] - totals["recursive"],
             totals["spawns_executed"], totals["spawns_static"]))
    ok = all(checks.values())
    print("checks: %s" % ("PASS" if ok else "FAIL"))
    for name, passed in checks.items():
        if not passed:
            print("  FAIL %s" % name)
    return ok


# --------------------------------------------------------------------------
# synthetic byte code (self-test)


def _enc_args(sig, args):
    out = bytearray()
    for ch, v in zip(sig, args):
        size = sbtvm.LETTER_SIZE[ch]
        if size == "str":
            out += str(v).encode("latin1") + b"\0"
        elif size == 8:
            out += struct.pack("<d", float(v))
        elif size == 1:
            out += bytes([int(v) & 0xFF])
        else:
            out += struct.pack("<I", int(v) & 0xFFFFFFFF)
    return bytes(out)


def enc_cmd(name, *args):
    for op, (nm, sig) in sbtvm.COMMANDS.items():
        if nm == name:
            return bytes([op]) + _enc_args(sig or "", args)
    raise KeyError(name)


def enc_cond(cid, *args):
    return bytes([cid]) + _enc_args(sbtvm.CONDITIONS[cid][1] or "", args)


def enc_eval(cid, *args):
    return b"\xc9" + enc_cond(cid, *args)


def enc_if(cid, args, body):
    # the size dword counts itself: the engine jumps by it from its own offset
    return (b"\xc7" + enc_cond(cid, *args) +
            struct.pack("<I", 4 + len(body)) + body)


def enc_else(body):
    """ELSE is a 5-byte jump placed last in the then-branch; *body* follows
    the IF (the compiler puts the false branch there), so only its length is
    needed to compute the join offset."""
    return b"\xc8" + struct.pack("<I", 4 + len(body))


class FakeScript:
    """Minimal stand-in for ``gm.sbt.Script`` (code + entity table)."""

    def __init__(self, code, rows):
        self.code = bytes(code)
        self.n = len(rows)
        self.handles = struct.pack(
            "<%dI" % (3 * self.n),
            *[0x3FF for _ in rows],
            *[eid & 0xFFFFFFFF for eid, _off in rows],
            *[off for _eid, off in rows])


THING_ABS = (100, 1, 2, 3, 0.0, 0.0, 0.0)       # type,x,y,z,3 doubles


def selftest():
    from gm.scene import NestingError, ThingContext
    out = []

    def check(name, ok, detail=""):
        out.append((name, bool(ok), detail))

    # 1. top level vs nested then-branch; the false branch is emitted as code
    #    after the IF (ELSE is the last instruction of the then-branch)
    else_code = enc_eval(2, 0, 0, 0, 102, 7, 8, 9, 0.0, 0.0, 0.0, 0)
    then_body = (enc_eval(1, 101, 4, 5, 6, 0.0, 0.0, 0.0) +
                 enc_else(else_code))
    code = (enc_eval(1, *THING_ABS) +
            enc_if(24, [0.5], then_body) + else_code + b"\xff")
    script = FakeScript(code, [(7, 0)])
    top = scenemod.things(script)
    rec = scenemod.things(script, recursive=True)
    check("default: then-branch hidden, else code is top-level",
          len(top) == 2 and [t.type_index for t in top] == [100, 102],
          str([(t.type_index, t.depth) for t in top]))
    check("recursive sees then + else code", len(rec) == 3, str(len(rec)))
    check("depth split", [t.depth for t in rec] == [0, 1, 0],
          str([t.depth for t in rec]))
    check("paths", [t.path for t in rec] == [(), ("IF RANDOM",), ()],
          str([t.path for t in rec]))
    check("sites unique", len({t.site for t in rec}) == 3)
    check("source entity", all(t.source == 7 for t in rec))
    check("absolute positions", rec[0].pos == (1, 2, 3) and rec[1].pos == (4, 5, 6),
          str([t.pos for t in rec]))

    # 1b. both branches are reachable when RANDOM is forked
    both = scenemod.execute_things(
        script, None, ThingContext(random_unknown=True))
    kinds = sorted(t.type_index for t in both.things)
    check("RANDOM forks then and else", kinds == [100, 101, 102], str(kinds))

    # 2. de-duplication when two entity rows name the same block
    script2 = FakeScript(code, [(7, 0), (8, 0)])
    check("recursive dedupe by site",
          len(scenemod.things(script2, recursive=True)) == 3,
          str(len(scenemod.things(script2, recursive=True))))

    # 3. nesting depth guard
    inner = enc_eval(1, *THING_ABS)
    for _ in range(70):
        inner = enc_if(24, [0.5], inner)
    deep = FakeScript(inner + b"\xff", [(1, 0)])
    try:
        scenemod.things(deep, recursive=True)
        check("depth guard", False, "no exception")
    except NestingError as exc:
        check("depth guard", "nesting deeper" in str(exc), str(exc))

    # 4. out-of-range IF body -> clean NestingError
    oob = b"\xc7" + enc_cond(24, 0.5) + struct.pack("<I", 0x1000)
    try:
        scenemod.things(FakeScript(oob, [(1, 0)]), recursive=True)
        check("OOB IF body", False, "no exception")
    except NestingError as exc:
        check("OOB IF body", "cannot decode block" in str(exc), str(exc))

    # 5. GENERATE cycle (runtime) must stop with a reason, not hang
    cyc = FakeScript(enc_cmd("GENERATE", 1) + b"\xff", [(1, 0)])
    t0 = time.monotonic()
    res = scenemod.execute_things(
        cyc, None, ThingContext(follow_generate=True, max_time=2.0))
    dt = time.monotonic() - t0
    check("GENERATE cycle is reported",
          res.reasons.get("recursion", 0) >= 1 and dt < 2.0,
          "%s in %.2fs" % (res.reasons, dt))

    # 6. context gates: difficulty and source player change the result
    gate = FakeScript(
        enc_if(6, [3], enc_eval(1, *THING_ABS)) + b"\xff", [(1, 0)])
    lo = scenemod.execute_things(gate, None, ThingContext(difficulty=1))
    hi = scenemod.execute_things(gate, None, ThingContext(difficulty=3))
    check("difficulty gate", len(lo.things) == 0 and len(hi.things) == 1,
          "%d/%d" % (len(lo.things), len(hi.things)))

    src = FakeScript(
        enc_if(9, [], enc_eval(1, *THING_ABS) +
               enc_else(enc_eval(1, 101, 4, 5, 6, 0.0, 0.0, 0.0))) +
        enc_eval(1, 101, 4, 5, 6, 0.0, 0.0, 0.0) + b"\xff",
        [(1, 0)])
    no = scenemod.execute_things(src, None, ThingContext())
    yes = scenemod.execute_things(src, None, ThingContext(source_player=0))
    check("source-player gate + ELSE",
          len(no.things) == 1 and no.things[0].type_index == 101
          and len(yes.things) == 1 and yes.things[0].type_index == 100,
          "%s / %s" % ([t.type_index for t in no.things],
                       [t.type_index for t in yes.things]))

    # 7. executed things carry the static nesting path
    check("executed path metadata",
          yes.things[0].via == "executed"
          and yes.things[0].path == ("IF SOURCE_IS_PLAYER",)
          and yes.things[0].site >= 0,
          str((yes.things[0].via, yes.things[0].path)))

    failed = sum(0 if ok else 1 for _n, ok, _d in out)
    for name, ok, detail in out:
        print("%s  %s%s" % ("ok  " if ok else "FAIL", name,
                            "" if ok else "  (%s)" % detail))
    print("\n%d tests, %d failed" % (len(out), failed))
    return 1 if failed else 0


# --------------------------------------------------------------------------
# main


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME",
                                                     DEFAULT_GAME))
    ap.add_argument("--level", action="append",
                    help="level stem or path (repeatable; default: all)")
    ap.add_argument("--check", action="store_true",
                    help="sweep the corpus and verify the counts")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--contexts", choices=("single", "all"), default="single")
    ap.add_argument("--difficulty", type=int, default=None)
    ap.add_argument("--players", type=int, default=None)
    ap.add_argument("--source-player", type=int, default=None)
    ap.add_argument("--god-mode", action="store_true")
    ap.add_argument("--objectives", action="store_true",
                    help="pretend every level objective is complete")
    ap.add_argument("--random-unknown", action="store_true")
    ap.add_argument("--reach", action="store_true",
                    help="add the two reach contexts (campaign diff 10 + all "
                         "objectives, dm + source player) to close the last 38 "
                         "static sites")
    ap.add_argument("--follow-generate", action="store_true",
                    help="follow GENERATE inside entity blocks (cycle test)")
    ap.add_argument("--max-time", type=float, default=None,
                    help="wall-clock budget per block (default 5 s)")
    ap.add_argument("--seed", type=int, default=0x12345678)
    args = ap.parse_args(argv)

    if args.self_test:
        return selftest()

    if args.level:
        paths = []
        for value in args.level:
            if os.path.isfile(value):
                paths.append(value)
            else:
                cand = os.path.join(args.game, value)
                if not os.path.isfile(cand):
                    cand += ".sbt"
                if not os.path.isfile(cand):
                    ap.error("level not found: %s" % value)
                paths.append(cand)
    else:
        paths = sorted(glob.glob(os.path.join(args.game, "*.sbt")))
    if not paths:
        print("no .sbt files in %s" % args.game, file=sys.stderr)
        return 1

    contexts = make_contexts(args)
    t0 = time.monotonic()
    rows, totals = analyse(paths, contexts)
    seconds = time.monotonic() - t0
    checks = (expectations(totals, contexts, full=len(paths) >= EXPECT_LEVELS)
              if args.check else {})
    ok = all(checks.values()) if checks else True

    if args.json:
        print(json.dumps({"levels": rows, "totals": totals,
                          "contexts": [c.label() for c in contexts],
                          "checks": checks, "ok": ok,
                          "seconds": round(seconds, 2)}, indent=1))
    else:
        print_report(rows, totals, checks, contexts)
        print("elapsed: %.1fs" % seconds)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
