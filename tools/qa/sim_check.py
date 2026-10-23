#!/usr/bin/env python3
"""Deterministic headless run check for `gunmetal --sim` (`M6.5.1`).

`--sim` builds the runtime from a shipped level (`vm_state_from_level` +
`GameRuntime::build`), steps the 91 Hz tick and prints a digest over every
thing position, every movement-automaton position and every door phase.  The
same build must produce the same digest twice; a change means a non-deterministic
tick (unordered iteration, an uninitialised field, a time-based branch).

Usage::

    python3 tools/qa/sim_check.py                       # dm1, 500 ticks
    python3 tools/qa/sim_check.py --level level0 --ticks 1000
    python3 tools/qa/sim_check.py --json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GAME = os.environ.get("GUNMETAL_DIR", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_BIN = str(ROOT / "build" / "sandbox" / "engine" / "gunmetal")


def run(binary: str, game: str, level: str, ticks: int) -> subprocess.CompletedProcess:
    return subprocess.run(
        [binary, "-g", game, "--sim", level, "--ticks", str(ticks)],
        capture_output=True, text=True)


def parse(text: str) -> dict:
    out: dict = {}
    match = re.search(r"sim: things=(\d+) \(alive (\d+), initial (\d+)\) "
                      r"doors=(\d+) switches=(\d+) lifts=(\d+) agents=(\d+)", text)
    if match:
        out["things"], out["alive"], out["initial"], out["doors"], out["switches"], \
            out["lifts"], out["agents"] = (int(value) for value in match.groups())
    match = re.search(r"sim: motion=stepped (\d+) moved (\d+) pushed (\d+)", text)
    if match:
        out["motion_stepped"], out["motion_moved"], out["motion_pushed"] = \
            (int(value) for value in match.groups())
    match = re.search(r"sim: digest=([0-9a-f]+)", text)
    if match:
        out["digest"] = match.group(1)
    out["ok"] = "sim: ok" in text
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="deterministic --sim check")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--binary", default=os.environ.get("GM_BIN", DEFAULT_BIN))
    ap.add_argument("--level", default="dm1")
    ap.add_argument("--ticks", type=int, default=500)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.game):
        print("sim_check: game directory not found: %s" % args.game, file=sys.stderr)
        return 2
    if not os.path.exists(args.binary):
        print("sim_check: engine binary not found: %s" % args.binary, file=sys.stderr)
        return 2

    first = run(args.binary, args.game, args.level, args.ticks)
    second = run(args.binary, args.game, args.level, args.ticks)
    if first.returncode != 0 or second.returncode != 0:
        print("sim_check: --sim failed (rc %d / %d):\n%s%s"
              % (first.returncode, second.returncode, first.stdout, first.stderr),
              file=sys.stderr)
        return 2

    one = parse(first.stdout)
    two = parse(second.stdout)
    problems = []
    if not one.get("ok") or not two.get("ok"):
        problems.append("the run did not print `sim: ok`")
    if "digest" not in one or "digest" not in two:
        problems.append("no digest in the output")
    elif one["digest"] != two["digest"]:
        problems.append("non-deterministic digest: %s != %s"
                        % (one["digest"], two["digest"]))
    if one.get("things") is None or one.get("initial") is None:
        problems.append("the summary line is missing")
    elif one["things"] < one["initial"]:
        problems.append("things=%s shrank below the initial %s"
                        % (one["things"], one["initial"]))
    if problems:
        for problem in problems:
            print("sim_check: FAIL %s" % problem, file=sys.stderr)
        return 1

    report = {"level": args.level, "ticks": args.ticks, "game": args.game,
              "digest": one["digest"], "deterministic": True, **one}
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        print("sim_check: %s ticks=%d things=%d alive=%d doors=%d switches=%d "
              "agents=%d" % (args.level, args.ticks, one["things"], one["alive"],
                             one["doors"], one["switches"], one["agents"]))
        print("sim_check: motion stepped=%d moved=%d digest=%s (twice, identical)"
              % (one.get("motion_stepped", 0), one.get("motion_moved", 0), one["digest"]))
        print("sim_check: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
