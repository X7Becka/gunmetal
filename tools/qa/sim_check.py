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


def run(binary: str, game: str, level: str, ticks: int,
        drive: str = "") -> subprocess.CompletedProcess:
    cmd = [binary, "-g", game, "--sim", level, "--ticks", str(ticks)]
    if drive:
        cmd += ["--sim-drive", drive]
    return subprocess.run(cmd, capture_output=True, text=True)


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
    match = re.search(r"sim: drive on_ground=(\d+) wall_ticks=(\d+) shots=(\d+) "
                      r"slot=(-?\d+) ammo=(\d+) shot_index=(\d+)", text)
    if match:
        out["on_ground"], out["wall_ticks"], out["shots"], out["slot"], \
            out["ammo"], out["shot_index"] = (int(value) for value in match.groups())
    match = re.search(r"sim: drive segments=(\d+) ticks=(\d+) pos=\(([^)]*)\) "
                      r"yaw=([-0-9.eE+]+)", text)
    if match:
        out["drive_segments"] = int(match.group(1))
        out["drive_ticks"] = int(match.group(2))
        out["drive_pos"] = match.group(3)
        out["drive_yaw"] = float(match.group(4))
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
    ap.add_argument("--drive", default="",
                    help="drive plan for --sim-drive (repeated runs must match)")
    ap.add_argument("--all", action="store_true",
                    help="run every shipped .sbt once (no determinism repeat)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.game):
        print("sim_check: game directory not found: %s" % args.game, file=sys.stderr)
        return 2
    if not os.path.exists(args.binary):
        print("sim_check: engine binary not found: %s" % args.binary, file=sys.stderr)
        return 2

    if args.all:
        levels = sorted(path.stem for path in Path(args.game).glob("*.sbt"))
        if not levels:
            print("sim_check: no .sbt under %s" % args.game, file=sys.stderr)
            return 2
        failed = []
        seconds = 0.0
        for level in levels:
            import time
            start = time.monotonic()
            proc = run(args.binary, args.game, level, args.ticks)
            seconds += time.monotonic() - start
            parsed = parse(proc.stdout)
            if proc.returncode != 0 or not parsed.get("ok"):
                failed.append((level, proc.returncode,
                               (proc.stdout + proc.stderr).strip().splitlines()[-1:]))
        if failed:
            for level, code, tail in failed:
                print("sim_check: FAIL %s rc=%d %s" % (level, code, tail), file=sys.stderr)
            return 1
        report = {"levels": len(levels), "ticks": args.ticks, "failed": 0,
                  "seconds": round(seconds, 1)}
        if args.json:
            print(json.dumps(report, ensure_ascii=False, indent=1))
        else:
            print("sim_check: %d levels, %d ticks, 0 failed (%.1f s)"
                  % (len(levels), args.ticks, seconds))
            print("sim_check: ok")
        return 0

    first = run(args.binary, args.game, args.level, args.ticks, args.drive)
    second = run(args.binary, args.game, args.level, args.ticks, args.drive)
    if first.returncode != 0 or second.returncode != 0:
        print("sim_check: --sim failed (rc %d / %d):\n%s%s"
              % (first.returncode, second.returncode, first.stdout, first.stderr),
              file=sys.stderr)
        return 2

    one = parse(first.stdout)
    two = parse(second.stdout)
    problems = []
    # The floor fix (2026-10-23): a driven player that walked forward must be
    # standing on the ground, not sinking through a reversed polygon.
    if args.drive and "forward=1" in args.drive and one.get("on_ground") == 0:
        problems.append("a forward plan ends with on_ground=0 (floor not found)")
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
              "drive": args.drive, "digest": one["digest"],
              "deterministic": True, **one}
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        print("sim_check: %s ticks=%d things=%d alive=%d doors=%d switches=%d "
              "agents=%d" % (args.level, args.ticks, one["things"], one["alive"],
                             one["doors"], one["switches"], one["agents"]))
        if args.drive:
            print("sim_check: drive segments=%d ticks=%d pos=(%s) yaw=%.6f shots=%d "
                  "ammo=%d" % (one.get("drive_segments", 0), one.get("drive_ticks", 0),
                               one.get("drive_pos", ""), one.get("drive_yaw", 0.0),
                               one.get("shots", 0), one.get("ammo", 0)))
        print("sim_check: motion stepped=%d moved=%d digest=%s (twice, identical)"
              % (one.get("motion_stepped", 0), one.get("motion_moved", 0), one["digest"]))
        print("sim_check: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
