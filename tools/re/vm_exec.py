#!/usr/bin/env python3
"""Execute a Gunmetal ``SCRIPT`` block and report what it builds.

The prototype interpreter is :mod:`gm.sbtvm_exec`; this tool is its CLI and
its corpus harness.  It runs the VM on a real ``.sbt`` level, prints a trace and
the resulting scene, or sweeps the whole shipped corpus and compares the
execution results with the static extractor :mod:`gm.scene`.

Usage::

    python3 vm_exec.py --level dm1                     # main block
    python3 vm_exec.py --level dm1 --script all        # every entity block
    python3 vm_exec.py --level dm1 --script id:-30077 --trace
    python3 vm_exec.py --level dm1 --json
    python3 vm_exec.py --selftest                      # no game files needed
    python3 vm_exec.py --corpus --compare              # all 36 .sbt
    python3 vm_exec.py --cmdtable                      # markdown table for docs

``.sbt`` files live outside the repository; ``GUNMETAL_GAME`` or ``--game``
selects the installation (default ``/mnt/d/games/Gunmetal 3dfx``).
"""
import argparse
import collections
import glob
import json
import os
import sys
import time
from dataclasses import asdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod            # noqa: E402
from gm import scene as scenemod       # noqa: E402
from gm import sbtvm                   # noqa: E402
from gm import sbtvm_exec as vm        # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: ``gm.scene`` category for every scene-building command
CATEGORY = {
    "ADD_SKY_LIGHT_SOURCE": "lights", "ADD_LIGHT_SOURCE": "lights",
    "ADD_LOCATION": "locations", "DELETE_LOCATION": "locations",
    "ADD_ITEM": "items", "ADD_ITEM_FIXED": "items",
    "ADD_SOUND_SOURCE": "sounds", "ADD_SPRITE": "sprites",
    "ADD_DOOR": "doors", "ADD_SWITCH": "switches", "ADD_LIFT": "lifts",
    "SET_PLAYER_START": "spawns", "SET_DEATHMATCH_START": "spawns",
}

CATEGORIES = ("lights", "locations", "items", "sounds", "sprites",
              "doors", "switches", "lifts", "spawns", "things")

# 0x83d2c8: 0 campaign, 1 normal, 2 classic, 3 hunt (docs/DM_RULES.md §3).
GAME_MODES = {"campaign": 0, "dm-normal": 1, "dm-classic": 2,
              "scavenger": 3}


# --------------------------------------------------------------------------
# static site sets (for --compare)


def static_sites(code):
    """Offsets of every scene-building instruction, including nested IFs."""
    top, _end = sbtvm.decode(code)
    out = collections.defaultdict(set)

    def walk(insns):
        for i in insns:
            if i.op == 0xC9 and i.args and i.args[0][0].startswith("ADD_THING"):
                out["things"].add(i.off)
            elif i.name in CATEGORY:
                out[CATEGORY[i.name]].add(i.off)
            if isinstance(getattr(i, "body", None), list):
                walk(i.body)

    walk(top)
    return out


def executed_sites(merged):
    return {cat: {getattr(o, "site", None) for o in getattr(merged, cat)}
            for cat in CATEGORIES}


# --------------------------------------------------------------------------
# execution helpers


def load_script(path):
    f = sbtmod.parse_file(path)
    recs = f.records_of(11)
    if not recs:
        raise SystemExit("%s: no SCRIPT record" % path)
    return f, recs[0].payload


def resolve_level(value, game):
    if os.path.isfile(value):
        return value
    for cand in (os.path.join(game, value),
                 os.path.join(game, value + ".sbt")):
        if os.path.isfile(cand):
            return cand
    raise SystemExit("level not found: %s" % value)


def block_offsets(script):
    """Entity-table offsets, in table order, de-duplicated."""
    out = []
    seen = set()
    for _eid, _flags, off in scenemod.entities(script):
        if off not in seen and 0 <= off < len(script.code):
            seen.add(off)
            out.append(off)
    return out


def select_scripts(script, spec):
    rows = scenemod.entities(script)
    if spec == "main":
        return [(None, 0)]
    if spec == "all":
        by_off = {}
        for eid, _flags, off in rows:
            by_off.setdefault(off, eid)
        return [(by_off.get(off), off) for off in block_offsets(script)]
    if spec.startswith("off:"):
        return [(None, int(spec[4:], 0))]
    if spec.startswith("id:"):
        want = int(spec[3:], 0)
        for eid, _flags, off in rows:
            if eid == want:
                return [(eid, off)]
        raise SystemExit("no entity with id %d" % want)
    try:
        idx = int(spec, 0)
    except ValueError:
        raise SystemExit("bad --script %r (main|all|<index>|id:<id>|off:<hex>)"
                         % spec)
    if not 0 <= idx < len(rows):
        raise SystemExit("entity index %d out of range (0..%d)"
                         % (idx, len(rows) - 1))
    eid, _flags, off = rows[idx]
    return [(eid, off)]


def make_executor(script, args, *, playertype=None, game_mode=None,
                  follow=None):
    st = vm.state_from_script(
        script,
        game_mode=GAME_MODES[args.game_mode] if game_mode is None else game_mode,
        player_count=args.players,
        difficulty=args.difficulty,
        level_number=args.level_number,
        god_mode=args.god_mode,
        players=max(1, args.players),
        seed=args.seed,
        playertype=playertype,
        source_player=args.source_player,
    )
    if follow is None:
        if args.no_follow:
            follow = False
        elif args.follow:
            follow = True
        else:
            follow = args.script != "all"
    return vm.Executor(
        st, max_steps=args.max_steps, max_time=args.max_time,
        max_paths=args.max_paths, on_unknown=args.on_unknown,
        follow_generate=follow, random_unknown=args.random_unknown,
        trace=args.trace, trace_limit=args.trace_limit,
    )


# --------------------------------------------------------------------------
# reporting


def run_one(path, args):
    _f, script = load_script(path)
    picks = select_scripts(script, args.script)
    ex = make_executor(script, args)
    results = []
    t0 = time.monotonic()
    per_block = []
    for eid, off in picks:
        rs = ex.run(off, source_entity=eid)
        per_block.append((eid, off, rs))
        results.extend(rs)
    elapsed = time.monotonic() - t0
    merged = vm.merge_scenes(results)
    reason = collections.Counter(r.reason for r in results)
    out = {
        "level": os.path.basename(path),
        "scripts": len(picks),
        "paths": len(results),
        "steps": sum(r.steps for r in results),
        "seconds": round(elapsed, 3),
        "reasons": dict(reason),
        "counts": merged.counts(),
        "notes": ex.notes[:20],
        "conditions": dict(sorted(ex.condition_counts.items())),
        "commands": {sbtvm.COMMANDS.get(op, ("OP_%d" % op, ""))[0]: n
                     for op, n in sorted(ex.command_counts.items())},
        "spawns": [{"index": s.index, "pos": s.pos, "angles": s.angles,
                    "kind": s.kind} for s in merged.spawns],
    }
    if args.trace:
        out["trace"] = []
        for eid, off, rs in per_block[:1]:
            for p in rs[:1]:
                out["trace"] = [asdict(t) for t in p.trace]
    if args.verbose:
        out["things"] = [{"kind": t.kind, "type": t.type_index,
                          "pos": t.pos, "owner": t.owner}
                         for t in merged.things]
        out["doors"] = [{"flags": d.flags, "ids": d.ids[:3],
                         "sector": d.params[0] if d.params else None,
                         "object": d.params[1] if len(d.params) > 1 else None}
                        for d in merged.doors]
        out["switches"] = [{"kind": s.kind, "id": s.id, "pos": s.pos}
                           for s in merged.switches]
    return out, per_block, ex


def print_summary(path, args, out, per_block):
    print("=== %s  (%d script%s, %d path%s, %d steps, %.2fs)"
          % (out["level"], out["scripts"], "" if out["scripts"] == 1 else "s",
             out["paths"], "" if out["paths"] == 1 else "s", out["steps"],
             out["seconds"]))
    for eid, off, rs in per_block[:args.show]:
        reasons = collections.Counter(r.reason for r in rs)
        tag = "entity %s" % eid if eid is not None else "main"
        print("  %-14s off=0x%-6x paths=%-4d %s"
              % (tag, off, len(rs),
                 ", ".join("%s x%d" % (k, v) for k, v in reasons.items())))
    if len(per_block) > args.show:
        print("  ... %d more blocks" % (len(per_block) - args.show))
    counts = {k: v for k, v in out["counts"].items() if v}
    print("  scene:", counts or "{}")
    if out["notes"]:
        print("  notes:")
        for n in out["notes"]:
            print("    -", n)
    if args.trace:
        for t in out.get("trace", [])[:args.trace_limit]:
            args_s = " ".join(_fmt(x) for x in t["args"])
            note = "  ; " + t["note"] if t["note"] else ""
            print("    %08x %s%s%s" % (t["pc"], "  " * t["depth"], t["name"],
                                       (" " + args_s) if args_s else "") + note)


def _fmt(v):
    if isinstance(v, str):
        return '"%s"' % v
    if isinstance(v, float):
        return "%g" % v
    return str(v)


# --------------------------------------------------------------------------
# selftest / command table


def do_selftest():
    failed = 0
    for name, ok, detail in vm.selftest():
        print("%s  %s%s" % ("ok  " if ok else "FAIL", name,
                            "" if ok else "  (%s)" % detail))
        failed += 0 if ok else 1
    print("\n%d tests, %d failed" % (len(vm.selftest()), failed))
    return 1 if failed else 0


def do_cmdtable():
    print("| op | command | signature | status | handler |")
    print("|---:|---|---|---|---|")
    for row in vm.command_table():
        print("| %d | `%s` | `%s` | %s | %s |"
              % (row["op"], row["name"], row["sig"], row["status"],
                 row["handler"]))
    return 0


# --------------------------------------------------------------------------
# corpus sweep


def _sweep_level(job):
    """Worker: run every picked block of one level (picklable job)."""
    path, args = job
    _f, script = load_script(path)
    picks = select_scripts(script, "all" if args.corpus_all else "main")
    modes = [(None, None)]
    if args.contexts == "all":
        modes = [(gm, pt) for gm in (0, 1) for pt in (0, 1, 2, 3)]
    merged = vm.Scene()
    blocks_done = blocks_all_done = blocks_end = 0
    reasons = collections.Counter()
    cmd_counts = collections.Counter()
    cond_counts = collections.Counter()
    for game_mode, playertype in modes:
        ex = make_executor(script, args, playertype=playertype,
                           game_mode=game_mode, follow=False)
        for eid, off in picks:
            rs = ex.run(off, source_entity=eid)
            blocks_done += 1
            for op, n in ex.command_counts.items():
                cmd_counts[sbtvm.COMMANDS.get(op, ("OP_%d" % op, ""))[0]] += n
            cond_counts.update(ex.condition_counts)
            ex.command_counts.clear()
            ex.condition_counts.clear()
            if all(r.reason in (vm.END, vm.PROGRAM_EXIT,
                                vm.LEVEL_TRANSITION) for r in rs):
                blocks_all_done += 1
            if any(r.reason == vm.END for r in rs):
                blocks_end += 1
            reasons.update(r.reason for r in rs)
            merged.extend(vm.merge_scenes(rs))
    merged = _collapse(merged)
    row = {
        "level": os.path.basename(path)[:-4],
        "blocks": blocks_done,
        "blocks_ended": blocks_all_done,
        "blocks_end": blocks_end,
        "paths": sum(reasons.values()),
        "reasons": dict(reasons),
        "counts": merged.counts(),
    }
    if args.compare:
        row.update(compare_level(_f, script, merged, args))
    return row, dict(cmd_counts), dict(cond_counts)


def sweep(paths, args):
    """Run the corpus, one process per level (the scene decode dominates)."""
    report = []
    totals = collections.Counter()
    cmd_totals = collections.Counter()
    cond_totals = collections.Counter()
    t0 = time.monotonic()
    jobs = [(path, args) for path in paths]
    workers = min(len(jobs), int(os.environ.get("GM_JOBS", "0")) or 16,
                  os.cpu_count() or 1)
    if workers > 1 and len(jobs) > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(_sweep_level, jobs))
    else:
        results = [_sweep_level(job) for job in jobs]
    for row, cmd_counts, cond_counts in results:
        report.append(row)
        totals.update(row["counts"])
        cmd_totals.update(cmd_counts)
        cond_totals.update(cond_counts)
    seconds = time.monotonic() - t0
    return report, totals, seconds, cmd_totals, cond_totals


def _collapse(merged):
    out = vm.Scene()
    seen = set()
    for cat in out.counts():
        for o in getattr(merged, cat):
            key = (cat, getattr(o, "site", None), getattr(o, "occ", None))
            if key in seen:
                continue
            seen.add(key)
            getattr(out, cat).append(o)
    return out


def compare_level(_f, script, merged, args):
    """Static vs executed comparison for one level."""
    static = static_sites(script.code)
    got = executed_sites(merged)
    missing = {cat: len(static.get(cat, set()) - got.get(cat, set()))
               for cat in CATEGORIES}
    # positions/models of the top-level ADD_THING family, as gm.scene sees it
    base = scenemod.from_sbt(_f)
    want = collections.Counter((t.kind, t.type_index, t.pos, t.owner)
                               for t in base.things)
    have = collections.Counter((t.kind, t.type_index, t.pos, t.owner)
                               for t in merged.things)
    spawn_want = collections.Counter((s.index, s.pos, s.angles)
                                     for s in base.spawns)
    spawn_have = collections.Counter((s.index, s.pos, s.angles)
                                     for s in merged.spawns)
    return {
        "static_missing": {k: v for k, v in missing.items() if v},
        "things_want": sum(want.values()),
        "things_got": sum(have.values()),
        "things_missing": sum((want - have).values()),
        "things_extra": sum((have - want).values()),
        "spawns_want": sum(spawn_want.values()),
        "spawns_got": sum(spawn_have.values()),
        "counts_static": {k: v for k, v in base.counts().items()
                          if k != "entities" and k != "other"},
    }


def print_corpus(report, totals, seconds, args):
    head = "%-9s %7s %7s %7s %5s" % ("level", "runs", "end", "terminal",
                                      "paths")
    if args.compare:
        head += " %6s %7s %7s %7s" % ("miss", "things", "spawns", "extra")
    print(head)
    for r in report:
        line = "%-9s %7d %7d %7d %5d" % (r["level"], r["blocks"],
                                         r["blocks_end"], r["blocks_ended"],
                                         r["paths"])
        if args.compare:
            miss = sum(r["static_missing"].values())
            line += " %6d %3d/%-3d %3d/%-3d %7d" % (
                miss, r["things_got"], r["things_want"],
                r["spawns_got"], r["spawns_want"], r["things_extra"])
        print(line)
    print("\n%d levels, %.1fs; totals: %s"
          % (len(report), seconds,
             ", ".join("%s=%d" % (k, v) for k, v in sorted(totals.items()))))
    # aggregate per-category missing
    miss = collections.Counter()
    reasons = collections.Counter()
    for r in report:
        miss.update(r.get("static_missing", {}))
        reasons.update(r["reasons"])
    if args.compare:
        print("static commands not executed:",
              ", ".join("%s=%d" % (k, v) for k, v in sorted(miss.items()))
              or "none")
        print("things: %d executed vs %d top-level in gm.scene; %d missing,"
              " %d extra (nested/conditional sites)"
              % (sum(r["things_got"] for r in report),
                 sum(r["things_want"] for r in report),
                 sum(r["things_missing"] for r in report),
                 sum(r["things_extra"] for r in report)))
        print("spawns: %d executed vs %d in gm.scene"
              % (sum(r["spawns_got"] for r in report),
                 sum(r["spawns_want"] for r in report)))
    print("blocks reaching END: %d/%d; terminal (END/exit/transition): %d"
          % (sum(r["blocks_end"] for r in report),
             sum(r["blocks"] for r in report),
             sum(r["blocks_ended"] for r in report)))
    print("stop reasons:", dict(reasons))
    diffs = [r for r in report if any(r.get("static_missing", {}).values())]
    for r in diffs[:12]:
        print("  %-9s %s" % (r["level"], r["static_missing"]))
    if len(diffs) > 12:
        print("  ... %d more" % (len(diffs) - 12))


# --------------------------------------------------------------------------
# main


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME",
                                                     DEFAULT_GAME))
    ap.add_argument("--level", help="level path or stem (e.g. dm1)")
    ap.add_argument("--script", default="main",
                    help="main | all | <index> | id:<id> | off:<hex>")
    ap.add_argument("--trace", action="store_true", help="instruction trace")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--cmdtable", action="store_true",
                    help="markdown command table")
    ap.add_argument("--corpus", nargs="?", const="", metavar="DIR",
                    help="run every .sbt (default: --game)")
    ap.add_argument("--corpus-all", action="store_true",
                    help="with --corpus: all entity blocks, not just main")
    ap.add_argument("--compare", action="store_true",
                    help="with --corpus: compare with gm.scene")
    ap.add_argument("--contexts", choices=("single", "all"), default="single",
                    help="with --corpus: single context or game modes x"
                         " player types")
    ap.add_argument("--max-steps", type=int, default=200_000)
    ap.add_argument("--max-time", type=float, default=5.0,
                    help="wall-clock budget per --script run (0 = none)")
    ap.add_argument("--max-paths", type=int, default=64)
    ap.add_argument("--seed", type=int, default=0x12345678)
    ap.add_argument("--on-unknown", choices=("stop", "false", "true", "both"),
                    default="both")
    ap.add_argument("--random-unknown", action="store_true",
                    help="treat RANDOM as unknown (explore both branches)")
    ap.add_argument("--follow", action="store_true",
                    help="execute GENERATE sub-scripts (default for one script)")
    ap.add_argument("--no-follow", action="store_true",
                    help="do not execute GENERATE sub-scripts")
    ap.add_argument("--game-mode", choices=sorted(GAME_MODES), default="campaign")
    ap.add_argument("--players", type=int, default=1)
    ap.add_argument("--difficulty", type=int, default=1)
    ap.add_argument("--level-number", type=int, default=0)
    ap.add_argument("--god-mode", action="store_true")
    ap.add_argument("--playertype", type=int, default=0)
    ap.add_argument("--source-player", type=int, default=None,
                    help="make the source a player (0..7)")
    ap.add_argument("--trace-limit", type=int, default=5000)
    ap.add_argument("--show", type=int, default=8,
                    help="blocks printed per level")
    ap.add_argument("--verbose", action="store_true",
                    help="include things/doors/switches in --json")
    args = ap.parse_args(argv)
    if args.no_follow:
        args.follow = False

    if args.selftest:
        return do_selftest()
    if args.cmdtable:
        return do_cmdtable()

    if args.corpus is not None:
        directory = args.corpus or args.game
        paths = sorted(glob.glob(os.path.join(directory, "*.sbt")))
        if not paths:
            print("no .sbt files in %s" % directory, file=sys.stderr)
            return 1
        report, totals, seconds, cmds, conds = sweep(paths, args)
        if args.json:
            print(json.dumps({"levels": report, "totals": dict(totals),
                              "commands": dict(cmds), "conditions": dict(conds),
                              "seconds": round(seconds, 2)}, indent=1))
        else:
            print_corpus(report, totals, seconds, args)
            print("commands executed: %d instructions, %d distinct"
                  % (sum(cmds.values()), len(cmds)))
            print("  " + ", ".join("%s=%d" % (k, v)
                                   for k, v in cmds.most_common(12)))
            print("conditions evaluated: %d, %d distinct"
                  % (sum(conds.values()), len(conds)))
        return 0

    if not args.level:
        ap.print_help()
        return 1
    path = resolve_level(args.level, args.game)
    out, per_block, _ex = run_one(path, args)
    if args.json:
        print(json.dumps(out, indent=1))
    else:
        print_summary(path, args, out, per_block)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
