#!/usr/bin/env python3
"""Mod regression: an edited `.sbt` must still load in the engine (M11.7).

The chain that a modder actually walks is

    disasm block -> edit text -> assemble -> replace in the container
                 -> load the level with the C++ engine

`tools/editor/sbtasm.py` and `tools/editor/sbtwrite.py` are tested on their own
(round-trip 15 061/15 061 and 36/36), but nothing pinned the *last* step: that
the bytes the writer produces are accepted by the engine.  This script does the
whole chain on a scratch copy and checks the engine's own summary.

It never writes into the installation: the level and its `.sbw` are copied to
``--work`` and only the copy is edited.

Usage::

    python3 tools/qa/mod_regress.py                       # level0, block 0x6f92
    python3 tools/qa/mod_regress.py --level dm1 --block 0x666e
    python3 tools/qa/mod_regress.py --json
    python3 tools/qa/mod_regress.py --self-test            # no game needed
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GAME = os.environ.get("GUNMETAL_DIR", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_BIN = str(ROOT / "build" / "sandbox" / "engine" / "gunmetal")
SBTASM = str(ROOT / "tools" / "editor" / "sbtasm.py")
SBTWRITE = str(ROOT / "tools" / "editor" / "sbtwrite.py")

#: The first `SET_CAMERA_HAZE` literal in the block; changing it keeps the
#: instruction the same size, so the container layout must not move.
EDIT_FROM = "SET_CAMERA_HAZE                    1.0 0.005"
EDIT_TO = "SET_CAMERA_HAZE                    0.5 0.005"


def run(cmd, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def engine_summary(binary: str, game: Path, level: str) -> tuple[int, str]:
    proc = run([binary, "-g", str(game), "-l", level, "--info"])
    return proc.returncode, (proc.stdout + proc.stderr)


def parse_summary(text: str) -> dict:
    """Pull the fields the engine prints for a level (stable summary lines)."""
    found = {}
    for line in text.splitlines():
        match = re.match(r"\s*entities:.*script=(\d+)/(\d+)", line)
        if match:
            found["scripts"] = int(match.group(1))
            found["code_bytes"] = int(match.group(2))
        match = re.match(r"\s*entities:.*records=(\d+)", line)
        if match:
            found["records"] = int(match.group(1))
        match = re.match(r"\s*world:.*bytes=(\d+)", line)
        if match:
            found["sbw_bytes"] = int(match.group(1))
    return found


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="mod regression (M11.7)")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--binary", default=os.environ.get("GM_BIN", DEFAULT_BIN))
    ap.add_argument("--level", default="level0")
    ap.add_argument("--block", default="0x6f92")
    ap.add_argument("--work", default="/tmp/opencode/mod-regress")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        sample = ("entities: records=8 poly=104 object=0 sector=26 sobs=67 "
                  "script=350/62497\nworld: textures=381 bytes=254439\n")
        parsed = parse_summary(sample)
        ok = parsed == {"scripts": 350, "code_bytes": 62497, "records": 8,
                        "sbw_bytes": 254439}
        print("self-test: 1/1 checks passed" if ok else "self-test: FAIL")
        return 0 if ok else 1

    game = Path(args.game)
    level = args.level
    src_sbt = game / (level + ".sbt")
    src_sbw = game / (level + ".sbw")
    if not src_sbt.is_file() or not src_sbw.is_file():
        print("mod_regress: %s.sbt/.sbw not found under %s" % (level, game),
              file=sys.stderr)
        return 2
    if not os.path.exists(args.binary):
        print("mod_regress: engine binary not found: %s" % args.binary,
              file=sys.stderr)
        return 2

    work = Path(args.work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    # 1. the engine's view of the untouched level (the expected structure)
    rc, text = engine_summary(args.binary, game, level)
    if rc != 0:
        print("mod_regress: the original level does not load (%d):\n%s"
              % (rc, text), file=sys.stderr)
        return 1
    before = parse_summary(text)
    if not before:
        print("mod_regress: cannot parse the engine summary:\n%s" % text,
              file=sys.stderr)
        return 1

    # 2. disassemble, edit one literal, assemble
    shutil.copy2(src_sbt, work / (level + ".sbt"))
    shutil.copy2(src_sbw, work / (level + ".sbw"))
    block_file = work / "block.asm"
    proc = run([sys.executable, SBTASM, "disasm", level, "--block", args.block,
                "--exact", "--game", str(game), "--out", str(block_file)])
    if proc.returncode != 0 or not block_file.is_file():
        print("mod_regress: disasm failed:\n%s" % (proc.stdout + proc.stderr),
              file=sys.stderr)
        return 1
    text_in = block_file.read_text(encoding="utf-8")
    if EDIT_FROM not in text_in:
        print("mod_regress: the block has no `%s` line" % EDIT_FROM,
              file=sys.stderr)
        return 1
    block_file.write_text(text_in.replace(EDIT_FROM, EDIT_TO, 1),
                          encoding="utf-8")

    # 3. replace the block in the container
    proc = run([sys.executable, SBTWRITE, "--script", str(work / (level + ".sbt")),
                "--block", args.block, "--asm", str(block_file),
                "--out", str(work / (level + ".sbt"))])
    if proc.returncode != 0:
        print("mod_regress: sbtwrite failed:\n%s" % (proc.stdout + proc.stderr),
              file=sys.stderr)
        return 1

    # 4. the engine must load the edited copy with the same structure
    rc, text = engine_summary(args.binary, work, level)
    after = parse_summary(text)
    if rc != 0 or after != before:
        print("mod_regress: edited level does not load the same way (%d)\n"
              "before=%s\nafter=%s\n%s" % (rc, before, after, text),
              file=sys.stderr)
        return 1

    # 5. the edit must be visible to the oracle (the literal is really in the file)
    sys.path.insert(0, str(ROOT))
    from gm import sbt, sbtvm

    parsed = sbt.parse_file(str(work / (level + ".sbt")))
    code = parsed.records_of(11)[0].payload.code
    values = []

    def walk(items):
        if not isinstance(items, (list, tuple)):
            return
        for ins in items:
            if not hasattr(ins, "op"):
                continue
            if ins.op == 80 and ins.args:  # SET_CAMERA_HAZE
                try:
                    values.append(float(ins.args[0]))
                except (TypeError, ValueError):
                    pass
            if getattr(ins, "body", None):
                walk(ins.body)

    instructions, _end = sbtvm.decode(code, 0, len(code))
    walk(instructions)
    if not any(abs(value - 0.5) < 1e-9 for value in values):
        print("mod_regress: the edited literal is not in the written file",
              file=sys.stderr)
        return 1

    report = {"level": level, "block": args.block, "game": str(game),
              "work": str(work), "engine": before,
              "haze_0_5": sum(1 for v in values if abs(v - 0.5) < 1e-9),
              "haze_total": len(values), "ok": True}
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        print("mod_regress: %s block %s -> %s" % (level, args.block, work))
        print("  engine: records=%d scripts=%d/%d sbw=%d B (unchanged)"
              % (before["records"], before["scripts"], before["code_bytes"],
                 before["sbw_bytes"]))
        print("  edit visible in the written .sbt: %d/%d SET_CAMERA_HAZE at 0.5"
              % (report["haze_0_5"], report["haze_total"]))
        print("  result: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
