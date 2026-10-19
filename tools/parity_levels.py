#!/usr/bin/env python3
"""Cross-check the C++ ``gunmetal --level --info`` dump against the Python readers.

Every shipped level is loaded by both implementations and the counters are
compared: ``.sbw`` tables/geometry, ``.sbt`` record histogram and ``.sbc``
lightmap pages.  This is the M3.3.3 guard against silent drift between
``gm/`` and ``engine/``.

Usage::

    python3 tools/parity_levels.py [--game DIR] [--gunmetal PATH]

The exit code is non-zero when any level differs.  Nothing is written to disk.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from gm import sbc, sbt, sbw  # noqa: E402  (needs sys.path first)

DEFAULT_GAME = Path("/mnt/d/games/Gunmetal 3dfx")
DEFAULT_BINARY = ROOT / "build" / "sandbox" / "engine" / "gunmetal"


def cpp_info(binary: Path, game: Path, level: str) -> dict[str, dict[str, object]]:
    proc = subprocess.run(
        [str(binary), "--game", str(game), "--level", level, "--info"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"gunmetal --level {level} failed:\n{proc.stderr or proc.stdout}")
    out: dict[str, dict[str, object]] = {}
    for line in proc.stdout.splitlines():
        key, _, rest = line.partition(":")
        if key == "world":
            out["world"] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", rest)}
        elif key == "entities":
            out["entities"] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", rest)}
            script = re.search(r"script=(\d+)/(\d+)", rest)
            if script:
                out["entities"]["script_handles"] = int(script.group(1))
                out["entities"]["script_code"] = int(script.group(2))
        elif key == "record types":
            out["types"] = {
                int(k): int(v) for k, v in re.findall(r"(\d+):(\d+)", rest)
            }
        elif key == "lightmaps":
            out["lightmaps"] = {
                "records": int(re.search(r"records=(\d+)", rest).group(1)),
                "texels": int(re.search(r"texels=(\d+)", rest).group(1)),
                "magic": re.search(r"magic=(\w+)", rest).group(1),
            }
    return out


def python_info(game: Path, level: str) -> dict[str, dict[str, object]]:
    world_path = game / f"{level}.sbw"
    if not world_path.exists():
        matches = [p for p in game.iterdir() if p.name.lower() == f"{level}.sbw".lower()]
        world_path = matches[0]
    lvl = sbw.parse(world_path.read_bytes())
    vertices = sum(len(o.vertices) for s in lvl.sectors for o in s.objects)
    info: dict[str, dict[str, object]] = {
        "world": {
            "textures": len(lvl.tex),
            "effects": len(lvl.effects),
            "sectors": len(lvl.sectors),
            "objects": lvl.n_objects,
            "vertices": vertices,
            "polygons": lvl.n_polys,
            "bytes": lvl.data_end,
        }
    }
    sbt_matches = [p for p in game.iterdir() if p.name.lower() == f"{level}.sbt".lower()]
    if sbt_matches:
        level_sbt = sbt.parse(sbt_matches[0].read_bytes())
        info["entities"] = {
            "records": len(level_sbt.records),
            "types": dict(Counter(r.type for r in level_sbt.records)),
        }
    sbc_matches = [p for p in game.iterdir() if p.name.lower() == f"{level}.sbc".lower()]
    if sbc_matches:
        maps = sbc.parse(sbc_matches[0].read_bytes())
        info["lightmaps"] = {
            "records": len(maps.records),
            "texels": sum(len(r.pixels) for r in maps.records),
            "magic": maps.magic.decode("ascii"),
        }
    return info


def compare(level: str, cpp: dict, py: dict) -> list[str]:
    problems: list[str] = []
    world = cpp.get("world", {})
    for key, expected in py["world"].items():
        got = world.get(key)
        if got != expected:
            problems.append(f"{level}: world.{key} cpp={got} py={expected}")
    if "entities" in py:
        ent = cpp.get("entities", {})
        for key in ("records",):
            got, expected = ent.get(key), py["entities"][key]
            if got != expected:
                problems.append(f"{level}: entities.{key} cpp={got} py={expected}")
        got_types = cpp.get("types", {})
        if got_types != py["entities"]["types"]:
            problems.append(
                f"{level}: record types cpp={got_types} py={py['entities']['types']}"
            )
    if "lightmaps" in py:
        lm = cpp.get("lightmaps", {})
        for key, expected in py["lightmaps"].items():
            got = lm.get(key)
            if got != expected:
                problems.append(f"{level}: lightmaps.{key} cpp={got} py={expected}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--gunmetal", type=Path, default=DEFAULT_BINARY)
    args = parser.parse_args()

    if not args.gunmetal.exists():
        print(f"parity: no engine binary at {args.gunmetal}", file=sys.stderr)
        return 2
    levels = sorted(
        {p.stem for p in args.game.iterdir() if p.suffix.lower() == ".sbw"},
        key=str.lower,
    )
    if not levels:
        print(f"parity: no .sbw files under {args.game}", file=sys.stderr)
        return 2

    failures: list[str] = []
    for level in levels:
        try:
            cpp = cpp_info(args.gunmetal, args.game, level)
            py = python_info(args.game, level)
        except Exception as exc:  # noqa: BLE001 - report and keep going
            failures.append(f"{level}: {exc}")
            continue
        failures.extend(compare(level, cpp, py))

    if failures:
        print(f"parity: {len(failures)} mismatch(es) over {len(levels)} levels")
        for line in failures:
            print("  " + line)
        return 1
    print(f"parity: {len(levels)}/{len(levels)} levels match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
