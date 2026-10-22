#!/usr/bin/env python3
"""Line coverage of the Python oracle, without third-party packages.

`regress.py` proves that the shipped corpus passes; it does not say which
branches never ran.  This tool runs a curated command set under the stdlib
`trace` module and aggregates `<module>.cover` files into a per-module table.

    python3 tools/qa/coverage.py                 # quick set (~1-2 min)
    python3 tools/qa/coverage.py --full          # + extraction commands
    python3 tools/qa/coverage.py --json out.json
    python3 tools/qa/coverage.py --keep DIR      # keep the .cover files

Interpretation: "exec" counts executable lines the tracer saw, "hit" those
executed at least once.  A branch can still be untested when its line is hit
(e.g. one arm of a conditional); treat the number as a map of blind spots,
not as a guarantee.  C++ coverage needs llvm-cov and is out of scope here.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GM = os.path.join(ROOT, "gm.py")
MIRROR = os.path.join(os.path.expanduser("~"), ".cache", "gunmetal-re", "game")
GAME = MIRROR if os.path.isdir(MIRROR) else "/mnt/d/games/Gunmetal 3dfx"

MISS_RE = re.compile(r"^>>>>>>\s")          # `trace --missing` marker
COUNT_RE = re.compile(r"^\s*(\d+):\s")      # executed line with a count


def commands(full: bool) -> list[list[str]]:
    """Curated invocations that touch most format modules."""
    cmds = [
        ["list", f"{GAME}/PICS/SBM.IDX"],
        ["inventory", "--game", GAME],
        ["sbt", GAME, "--resolve"],
        ["sbw", f"{GAME}/dm1.sbw"],
        ["sbc", f"{GAME}/level0.sbc", "/tmp/opencode/cov-sbc", "--check"],
        ["scene", f"{GAME}/dm1.sbt", "--json"],
        ["sob", f"{GAME}/SOBS/20mmclp.sob", "/tmp/opencode/cov-sob", "--check"],
        ["soa", f"{GAME}/Sobanim/Cultist.soo"],
        ["sou", f"{GAME}/sounds/SOU.IDX", "--json"],
        ["sml", GAME],
        ["sbv", f"{GAME}/Vid/Intro.sbv", "--list-only"],
        ["film", f"{GAME}/Film/AMMO4.SOF", "--json"],
        ["save", f"{GAME}/SAVEGAME/saved0.sbt", "--json"],
        ["campaign", "--game", GAME],
        ["misc", f"{GAME}/GUNMETAL.CFG", "--json"],
    ]
    if full:
        cmds += [
            ["textures", GAME, "/tmp/opencode/cov-tex", "--force"],
            ["sprites", GAME, "/tmp/opencode/cov-spr", "--force"],
            ["frontend", GAME, "/tmp/opencode/cov-fe", "--force"],
            ["sob", f"{GAME}/SOBS", "/tmp/opencode/cov-models", "--force"],
        ]
    return cmds


def run_one(args: list[str], coverdir: str) -> tuple[int, str]:
    cmd = [sys.executable, "-m", "trace", "--count", "--missing",
           "--coverdir", coverdir,
           "--ignore-dir", "/usr", "--ignore-dir", os.path.expanduser("~/tools"),
           GM, *args]
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    return p.returncode, (p.stdout + p.stderr)


def collect(coverdir: str) -> dict[str, dict[int, bool]]:
    """``relpath -> {line_no: hit}`` for executable lines in one run.

    A ``.cover`` file lists every source line in order; lines starting with a
    count or ``>>>>>>`` are executable, the rest (comments/docstrings) are not.
    Keeping the line number lets several runs be unioned without
    double-counting executable lines.
    """
    out: dict[str, dict[int, bool]] = {}
    for path in glob.glob(os.path.join(coverdir, "**", "*.cover"),
                          recursive=True):
        # `trace` names cover files by *module*: gm.cover is gm.py,
        # gm.sbw.cover is gm/sbw.py (packages imported from this repo).
        stem = os.path.basename(path)
        if stem.endswith(".cover"):
            stem = stem[:-len(".cover")]
        if stem == "gm":
            rel = "gm.py"
        elif stem.startswith("gm."):
            rel = "gm/" + stem[len("gm."):].replace(".", "/") + ".py"
        else:
            continue
        lines = out.setdefault(rel, {})
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for no, line in enumerate(fh, 1):
                    if MISS_RE.match(line):
                        lines.setdefault(no, False)      # executable, not run
                    elif COUNT_RE.match(line):
                        lines[no] = True                 # executed at least once
        except OSError:
            continue
    return out


def merge(into: dict[str, dict[int, bool]],
          part: dict[str, dict[int, bool]]) -> None:
    for name, lines in part.items():
        dst = into.setdefault(name, {})
        for no, hit in lines.items():
            dst[no] = dst.get(no, False) or hit


def main(argv=None) -> int:
    global GAME
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--full", action="store_true",
                    help="also run the extraction commands (slower)")
    ap.add_argument("--json", help="write the table as JSON")
    ap.add_argument("--keep", help="keep .cover files in this directory")
    ap.add_argument("--show-missing", type=int, default=40,
                    help="lines printed by --missing (default 40)")
    ap.add_argument("--show", type=int, default=12,
                    help="modules printed in the summary (default 12)")
    ap.add_argument("--missing", metavar="MODULE",
                    help="list unexecuted line numbers for one module "
                         "(e.g. gm/sbw.py or gm.sbw)")
    ap.add_argument("--min", type=float, default=0.0,
                    help="exit 1 when total coverage is below this percentage")
    opts = ap.parse_args(argv)

    GAME = opts.game
    tmp = opts.keep or tempfile.mkdtemp(prefix="gm-cov-", dir="/tmp/opencode")
    os.makedirs(tmp, exist_ok=True)
    failed = []
    merged: dict[str, dict[int, bool]] = {}
    for index, args in enumerate(commands(opts.full)):
        run_dir = os.path.join(tmp, "run%02d" % index)
        os.makedirs(run_dir, exist_ok=True)
        rc, text = run_one(args, run_dir)
        merge(merged, collect(run_dir))
        if rc != 0:
            failed.append((args[0], rc, text.strip().splitlines()[-1:]))
        print("  %-12s rc=%d" % (args[0], rc), file=sys.stderr, flush=True)

    rows = {name: [sum(1 for hit in lines.values() if hit), len(lines)]
            for name, lines in merged.items()}
    if not rows:
        print("coverage: no .cover files collected", file=sys.stderr)
        return 1
    total_hit = sum(v[0] for v in rows.values())
    total_exec = sum(v[1] for v in rows.values())
    table = [{"module": name, "hit": v[0], "exec": v[1],
              "pct": round(100.0 * v[0] / v[1], 1) if v[1] else 0.0}
             for name, v in sorted(rows.items())]
    if opts.json:
        with open(opts.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"files": table, "hit": total_hit, "exec": total_exec,
                       "pct": round(100.0 * total_hit / total_exec, 1),
                       "failed": [f[0] for f in failed]}, fh, indent=2)
        print("coverage: %d/%d lines (%.1f%%), %d file(s) -> %s"
              % (total_hit, total_exec, 100.0 * total_hit / total_exec,
                 len(table), opts.json))
    else:
        for row in sorted(table, key=lambda r: r["pct"])[:opts.show]:
            print("  %-22s %5d/%-5d %5.1f%%" % (row["module"], row["hit"],
                                                row["exec"], row["pct"]))
        print("coverage: %d/%d executable lines (%.1f%%) in %d files"
              % (total_hit, total_exec, 100.0 * total_hit / total_exec,
                 len(table)))
    if failed:
        print("coverage: %d command(s) failed: %s"
              % (len(failed), ", ".join(f[0] for f in failed)),
              file=sys.stderr)
    if opts.keep:
        print("coverage: cover files kept in %s" % tmp, file=sys.stderr)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    if opts.missing:
        key = opts.missing
        if key.endswith(".py"):
            key = key if "/" in key else "gm/" + key
        elif key.startswith("gm."):
            key = "gm/" + key[3:].replace(".", "/") + ".py"
        else:
            key = "gm/" + key.replace(".", "/") + ".py"
        if key not in merged:
            print("coverage: %s was not traced in this run" % opts.missing,
                  file=sys.stderr)
        else:
            lines = merged[key]
            missing = [no for no, hit in sorted(lines.items()) if not hit]
            print("%s: %d unexecuted of %d executable lines"
                  % (key, len(missing), len(lines)))
            source = []
            try:
                with open(os.path.join(ROOT, key), "r", encoding="utf-8",
                          errors="replace") as fh:
                    source = fh.read().splitlines()
            except OSError:
                pass
            for no in missing[:opts.show_missing]:
                text = source[no - 1].strip() if no <= len(source) else ""
                print("  %5d  %s" % (no, text[:120]))
            if len(missing) > opts.show_missing:
                print("  ... %d more" % (len(missing) - opts.show_missing))
    total_pct = 100.0 * total_hit / total_exec
    if opts.min and total_pct < opts.min:
        print("coverage: %.1f%% is below --min %.1f%%" % (total_pct, opts.min),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
