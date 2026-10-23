#!/usr/bin/env python3
"""Performance snapshot (M12.3): load, render and decode timings.

Report-only by default: the numbers depend on the machine and on the
filesystem (`/mnt/c` vs a Linux cache), so they are facts to compare over time,
not a gate.  ``--max-render-ms`` adds an explicit budget for a machine where a
regression should fail.

What is measured (all read-only):

    parse      `gm.sbt`/`gm.sbw` for level0 and level24 (Python reference)
    vm         `gm.sbtvm_exec` main block of level0 (Python reference)
    render     the C++ binary `--render` at 320x200 and 640x400
    decode     one SOU1 sample end to end (`gm.sou1`)
    server     `opencode`-independent: nothing.

Child RSS is the peak of the whole child process (`ru_maxrss`), so it includes
the engine's textures/geometry, not just the level file.

Usage::

    python3 tools/qa/perf.py                 # table
    python3 tools/qa/perf.py --json
    python3 tools/qa/perf.py --repeat 5 --render-ms 100
    python3 tools/qa/perf.py --self-test     # no game needed
"""
from __future__ import annotations

import argparse
import json
import os
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_BIN = str(ROOT / "build" / "sandbox" / "engine" / "gunmetal")


def _median(values):
    return statistics.median(values) if values else 0.0


def _timed(fn, repeat):
    """Run *fn* `repeat` times, return (median_ms, min_ms)."""
    times = []
    for _ in range(max(1, repeat)):
        start = time.perf_counter()
        fn()
        times.append((time.perf_counter() - start) * 1000.0)
    return _median(times), min(times)


def measure_parse(game: Path, levels, repeat: int) -> dict:
    from gm import sbt, sbw

    rows = {}
    for name in levels:
        sbt_path = game / (name + ".sbt")
        sbw_path = game / (name + ".sbw")
        if not sbt_path.exists():
            continue

        def load():
            sbt.parse_file(str(sbt_path))
            if sbw_path.exists():
                sbw.parse_file(str(sbw_path))

        median, best = _timed(load, repeat)
        rows[name] = {"ms": round(median, 2), "best_ms": round(best, 2),
                      "sbt_bytes": sbt_path.stat().st_size,
                      "sbw_bytes": sbw_path.stat().st_size if sbw_path.exists() else 0}
    return rows


def measure_vm(game: Path, level: str, repeat: int) -> dict:
    from gm import sbt, sbtvm_exec

    path = game / (level + ".sbt")
    if not path.exists():
        return {}
    parsed = sbt.parse_file(str(path))
    script = parsed.records_of(11)[0].payload

    def run():
        state = sbtvm_exec.state_from_script(script)
        executor = sbtvm_exec.Executor(state, max_time=0.0)
        executor.run(0)

    median, best = _timed(run, repeat)
    return {"level": level, "ms": round(median, 2), "best_ms": round(best, 2)}


def measure_render(binary: str, game: Path, level: str, sizes, repeat: int) -> dict:
    if not (os.path.exists(binary) and (game / (level + ".sbt")).exists()):
        return {}
    rows = {}
    with tempfile.TemporaryDirectory(prefix="gm-perf-") as work:
        for width, height in sizes:
            out = Path(work) / ("%dx%d.png" % (width, height))

            def render():
                subprocess.run([binary, "--game", str(game), "--level", level,
                                "--render", str(out),
                                "--width", str(width), "--height", str(height)],
                               check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)

            median, best = _timed(render, repeat)
            rows["%dx%d" % (width, height)] = {
                "ms": round(median, 2), "best_ms": round(best, 2),
                "cold_per_s": round(1000.0 / median, 2) if median else 0.0}
    return rows


def measure_cli_load(binary: str, game: Path, level: str, repeat: int) -> dict:
    """`gunmetal -l LEVEL --info`: C++ parse/load without textures or drawing."""
    if not (os.path.exists(binary) and (game / (level + ".sbt")).exists()):
        return {}

    def load():
        subprocess.run([binary, "--game", str(game), "-l", level, "--info"],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    median, best = _timed(load, repeat)
    return {"level": level, "ms": round(median, 2), "best_ms": round(best, 2)}


def measure_decode(game: Path, repeat: int) -> dict:
    from gm import bigfile, sou1

    index = game / "sounds" / "SOU.IDX"
    if not index.exists():
        return {}
    archive = bigfile.BigFile(str(index))
    data = Path(archive.dat_path).read_bytes()
    entry = next((e for e in archive.entries if e.name.upper().startswith("ALARMK")), None)
    if entry is None:
        return {}
    blob = entry.data(data)
    frames = {}

    def decode():
        frames["sound"] = sou1.decode_entry(blob)

    median, best = _timed(decode, repeat)
    rate, channels, slots = frames.get("sound", (0, 0, []))
    return {"sample": "ALARMK", "bytes": len(blob), "ms": round(median, 3),
            "best_ms": round(best, 3), "rate": rate, "channels": channels,
            "slots": len(slots), "frames": len(slots[0]) if slots else 0}


def child_peak_rss_mb() -> float:
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return round(usage.ru_maxrss / 1024.0, 1)  # ru_maxrss is KiB on Linux


def run(game: Path, binary: str, repeat: int) -> dict:
    report = {"game": str(game), "binary": binary, "repeat": repeat,
              "parse": measure_parse(game, ("level0", "dm1", "level24"), repeat),
              "vm": measure_vm(game, "level0", repeat),
              "cli_load": measure_cli_load(binary, game, "level0", repeat),
              "render": measure_render(binary, game, "level0",
                                       ((320, 200), (640, 400)), repeat),
              "decode": measure_decode(game, repeat),
              "child_peak_rss_mb": child_peak_rss_mb()}
    return report


def print_report(report: dict) -> None:
    print("parse (Python reference)")
    for name, row in report["parse"].items():
        print("  %-8s %7.2f ms (best %6.2f)  sbt %d B, sbw %d B"
              % (name, row["ms"], row["best_ms"], row["sbt_bytes"], row["sbw_bytes"]))
    vm = report.get("vm") or {}
    if vm:
        print("vm (main block of %s)" % vm["level"])
        print("  %7.2f ms (best %6.2f)" % (vm["ms"], vm["best_ms"]))
    load = report.get("cli_load") or {}
    if load:
        print("load (C++ `-l %s --info`: parse + summary)" % load["level"])
        print("  %7.2f ms (best %6.2f)" % (load["ms"], load["best_ms"]))
    if report["render"]:
        print("render (cold: load + one frame + PNG encode; `cold_per_s` is"
              " complete processes per second, not an FPS)")
        for size, row in report["render"].items():
            print("  %-8s %7.2f ms (best %6.2f) -> %.2f cold renders/s"
                  % (size, row["ms"], row["best_ms"], row["cold_per_s"]))
    dec = report.get("decode") or {}
    if dec:
        print("SOU1 decode (%s: %d bytes, %s Hz, %d ch, %d frames)"
              % (dec["sample"], dec["bytes"], dec["rate"], dec["channels"],
                 dec["frames"]))
        print("  %7.3f ms (best %6.3f)" % (dec["ms"], dec["best_ms"]))
    print("child peak RSS: %.1f MB" % report["child_peak_rss_mb"])


def _self_test() -> int:
    failures = 0

    def check(cond, what):
        nonlocal failures
        if not cond:
            print("FAIL %s" % what)
            failures += 1

    check(_median([3.0, 1.0, 2.0]) == 2.0, "median of three")
    check(_median([]) == 0.0, "median of nothing")
    check(_median([5.0]) == 5.0, "median of one")
    median, best = _timed(lambda: None, 3)
    check(median >= 0.0 and best >= 0.0 and best <= median + 1e-9,
          "timed: best <= median")
    check(child_peak_rss_mb() >= 0.0, "rss is not negative")
    print("self-test: 5/5 checks passed")
    return 0 if failures == 0 else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="performance snapshot (M12.3)")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--binary", default=os.environ.get("GM_BIN", DEFAULT_BIN))
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--render-ms", type=float, default=0.0,
                    help="fail when a render median exceeds this budget")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    game = Path(args.game)
    if not game.is_dir():
        print("perf: game directory not found: %s" % game, file=sys.stderr)
        return 2
    binary = args.binary
    if not os.path.exists(binary):
        found = shutil.which("gunmetal")
        binary = found or binary
    report = run(game, binary, args.repeat)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        print_report(report)
    if args.render_ms > 0:
        slow = {size: row for size, row in report["render"].items()
                if row["ms"] > args.render_ms}
        if slow:
            for size, row in sorted(slow.items()):
                print("perf: render %s %.2f ms > %.2f ms budget"
                      % (size, row["ms"], args.render_ms), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
