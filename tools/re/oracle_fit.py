#!/usr/bin/env python3
"""Fit the camera of an oracle frame: search pitch/yaw/focal, score with corr.

Why
---
The sidecar camera of ``win1..win3`` does not reproduce the captured frame
(``docs/ORACLE.md``): ``win2`` is close, ``win1``/``win3`` are not, and the
disagreement survives every shading fix (light curve proven linear, palette and
texture indices corrected).  The remaining candidates are (a) the sidecar
camera is not exactly the render camera — e.g. the view pitch is stored before
a recoil/shake term, or the recorded pitch is the *body* pitch — and (b) the
vertical focal normalisation (``160,83.3`` vs ``160,133.3``, ``M4.4``).

Instead of guessing, this tool *measures*: it renders a grid of camera variants
around the sidecar state and reports the best correlation with the capture.  A
flat maximum means "the camera is right, something else is wrong"; a sharp
maximum at a non-zero offset means "the sidecar is missing that term".

    python3 tools/re/oracle_fit.py --sidecar out/oracle/win1.json \
        --oracle out/oracle/win1.png --yaw -6:6:2 --pitch -6:6:2 --json

Grid syntax: ``start:stop:step`` in degrees (pitch/yaw) or pixels (focal).

Scoring
-------
Only the ``world`` band is scored (``oracle_compare.metrics``, top 70 %), the
same band `oracle_render.py` prints.  ``corr`` (Pearson on luma) is the primary
number; ``edges`` (Jaccard of Sobel masks) is reported as the tie-breaker.

The tool never writes to the game installation and never touches the capture.

Self-test
---------
``--self-test`` is fully synthetic: a shifted gradient pattern is used as the
"oracle", the search must recover the shift (no game data, no PNG on disk).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import oracle_compare as oc  # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME",
                              "/mnt/d/games/Gunmetal 3dfx")
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")


def parse_grid(spec: str, default: float) -> list[float]:
    """``"start:stop:step"`` -> inclusive list; ``""`` -> ``[default]``."""
    spec = (spec or "").strip()
    if not spec:
        return [default]
    parts = spec.split(":")
    if len(parts) == 1:
        return [float(parts[0])]
    if len(parts) != 3:
        raise ValueError(f"bad grid {spec!r}: want start:stop:step")
    start, stop, step = (float(v) for v in parts)
    if step <= 0:
        raise ValueError(f"bad grid {spec!r}: step must be positive")
    count = int(math.floor((stop - start) / step + 1e-9)) + 1
    return [round(start + i * step, 6) for i in range(max(1, count))]


def forward_from(pitch: float, yaw: float) -> tuple[float, float, float]:
    """Engine basis ``0x41e025`` (docs/CAMERA.md): ``(sy*cp, cy*cp, -sp)``."""
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    return (sy * cp, cy * cp, -sp)


def look_from(eye, pitch: float, yaw: float, distance: float = 100.0):
    fx, fy, fz = forward_from(pitch, yaw)
    return (eye[0] + fx * distance, eye[1] + fy * distance,
            eye[2] + fz * distance)


def score_frames(oracle, ours, world_end: float = 0.7) -> dict:
    """The two numbers the search uses, from oracle_compare.metrics."""
    m = oc.metrics(oracle, ours, world_end)
    world = m.get("world", {})
    return {"corr": float(world.get("corr", 0.0)),
            "edges": float(world.get("edges", 0.0)),
            "mad": float(world.get("mad", 0.0))}


def fit(score, combos, progress=None, origin=None) -> list[dict]:
    """Run ``score(combo) -> dict`` over ``combos``, best ``corr`` first.

    ``combos`` are ``(yaw, pitch, focal_y, eye_z)`` tuples.  Ties on ``corr``
    are broken by ``edges`` (higher is better), then by the distance from
    ``origin`` (the sidecar camera), so a flat maximum prefers "no change".
    The distance is measured in degrees plus ``|eye_z| / 10``, i.e. 10 world
    units of eye height count as one degree (they are nearly degenerate for
    distant geometry, see the module docstring).
    """
    if origin is None:
        origin = (0.0, 0.0, 0.0)
    rows = []
    for i, combo in enumerate(combos, 1):
        yaw, pitch, focal_y, eye_z = combo
        s = score(combo)
        rows.append({"yaw": yaw, "pitch": pitch, "focal_y": focal_y,
                     "eye_z": eye_z,
                     "corr": s["corr"], "edges": s["edges"],
                     "mad": s["mad"]})
        if progress is not None:
            progress(i, len(combos), rows[-1])
    oy, op, oz = origin[0], origin[1], origin[2]
    rows.sort(key=lambda r: (-r["corr"], -r["edges"],
                             abs(r["yaw"] - oy) + abs(r["pitch"] - op)
                             + abs(r["eye_z"] - oz) / 10.0))
    return rows


def synth_frame(width: int, height: int, shift: int) -> tuple[int, int, bytes]:
    """A vertical gradient with a bright bar; ``shift`` moves the bar in x."""
    px = bytearray(width * height * 3)
    for y in range(height):
        base = int(30 + 150 * y / max(1, height - 1))
        for x in range(width):
            v = base
            if (x - shift) % 37 < 6:
                v = min(255, base + 60)
            i = (y * width + x) * 3
            px[i] = px[i + 1] = px[i + 2] = v
    return width, height, bytes(px)


def selftest() -> int:
    width, height = 64, 48
    truth = 5
    oracle = synth_frame(width, height, truth)
    # The "renderer" here shifts the pattern by the yaw parameter (1 px/deg).
    def score(combo):
        yaw, _pitch, _focal, _dz = combo
        ours = synth_frame(width, height, int(round(yaw)))
        s = score_frames(oracle, ours, WORLD_END)
        return s
    rows = fit(score, [(y, 0.0, 0.0, 0.0) for y in range(-8, 9)])
    ok = True
    best = rows[0]
    if best["yaw"] != truth:
        print(f"FAIL: best yaw {best['yaw']} != {truth}")
        ok = False
    if best["corr"] < 0.999:
        print(f"FAIL: best corr {best['corr']:.3f} < 0.999")
        ok = False
    shifted = [r for r in rows if r["yaw"] == truth][0]
    wrong = [r for r in rows if abs(r["yaw"] - truth) >= 3]
    if wrong and max(r["corr"] for r in wrong) >= shifted["corr"]:
        print("FAIL: a wrong offset scored at least as high")
        ok = False
    # Grid parsing: inclusive, integer step, single value, default.
    if parse_grid("-6:6:6", 0.0) != [-6.0, 0.0, 6.0]:
        print("FAIL: parse_grid -6:6:6")
        ok = False
    if parse_grid("", 1.5) != [1.5] or parse_grid("2.5", 0.0) != [2.5]:
        print("FAIL: parse_grid single/default")
        ok = False
    # Forward basis: pitch 0, yaw 0 -> +Y; pitch +90 deg -> -Z (up is +Z).
    fx, fy, fz = forward_from(0.0, 0.0)
    if abs(fx) > 1e-12 or abs(fy - 1.0) > 1e-12 or abs(fz) > 1e-12:
        print("FAIL: forward at yaw=pitch=0")
        ok = False
    fx, fy, fz = forward_from(math.pi / 2, 0.0)
    if abs(fz + 1.0) > 1e-9:
        print("FAIL: forward at pitch=+90 (engine pitch is +up)")
        ok = False
    # Ties prefer "no change": identical scores must sort 0 before 3.
    rows2 = fit(lambda c: {"corr": 0.5, "edges": 0.1, "mad": 0.2},
                [(3.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 0.0)],
                origin=(0.0, 0.0, 0.0))
    if rows2[0]["yaw"] != 0.0:
        print("FAIL: tie-break did not prefer the sidecar camera")
        ok = False
    print(f"selftest: {'ok' if ok else 'FAILED'} (10 checks)")
    return 0 if ok else 1


WORLD_END = 0.7


def resolve_level(game: str, name: str) -> tuple[str, str]:
    """``LEVEL0`` -> ``(level0.sbw, level0.sbc)``, case-insensitively."""
    for root in (game, MIRROR):
        if not os.path.isdir(root):
            continue
        found = {}
        for entry in os.listdir(root):
            stem, ext = os.path.splitext(entry)
            if stem.lower() == name.lower():
                found[ext.lower()] = os.path.join(root, entry)
        if ".sbw" in found:
            return found[".sbw"], found.get(".sbc", "")
    raise FileNotFoundError(f"level {name!r} not found under {game!r}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sidecar", help="oracle state JSON (out/oracle/winN.json)")
    ap.add_argument("--oracle", help="reference PNG (out/oracle/winN.png)")
    ap.add_argument("--out", help="best-fit PNG (optional, keep out of git)")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=200)
    ap.add_argument("--yaw", default="0",
                    help="pitch grid offset in degrees (default 0)")
    ap.add_argument("--pitch", default="0",
                    help="pitch grid offset in degrees (default 0)")
    ap.add_argument("--eye-z", default="0",
                    help="eye height offset grid in world units (default 0)")
    ap.add_argument("--focal-y", default="133.3333",
                    help="focal_y grid in pixels (default 133.3333)")
    ap.add_argument("--focal-x", type=float, default=None,
                    help="fixed focal_x (default: width/2)")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--keep-border", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--self-test", dest="selftest", action="store_true")
    opts = ap.parse_args(argv)

    if opts.selftest:
        return selftest()
    if not opts.sidecar or not opts.oracle:
        ap.error("--sidecar and --oracle are required (or --self-test)")

    import oracle_render as orr
    from gm import png, render

    side = orr.load_sidecar(opts.sidecar)
    frame = orr.camera_frame(side["camera"], "auto")
    eye = frame["eye"]
    base_pitch, base_yaw = frame["pitch"], frame["yaw"]
    fx = opts.focal_x if opts.focal_x is not None else opts.width / 2.0

    sbw_path, sbc_path, meshes = orr.build_scene(opts.game, side["level"], True)
    pool = render.TexturePool(opts.game)

    ref = oc.load_rgb(opts.oracle)
    border = 0 if opts.keep_border else orr.ORACLE_BORDER_PX
    if border:
        ref = oc.crop(ref, (border, border, ref[0] - 2 * border,
                            ref[1] - 2 * border))
    ref = oc.resize_area(ref, opts.width, opts.height)

    yaws = parse_grid(opts.yaw, 0.0)
    pitches = parse_grid(opts.pitch, 0.0)
    focals = parse_grid(opts.focal_y, 133.3333)
    zs = parse_grid(opts.eye_z, 0.0)
    combos = [(base_yaw + math.radians(dy), base_pitch + math.radians(dp), fy, dz)
              for dy in yaws for dp in pitches for fy in focals for dz in zs]

    def render_at(combo):
        yaw, pitch, focal_y, dz = combo
        eye2 = (eye[0], eye[1], eye[2] + dz)
        look = look_from(eye2, pitch, yaw)
        return render.render(meshes, width=opts.width, height=opts.height,
                             pool=pool, pos=eye2, look=look,
                             focal=(fx, focal_y), lightmap=True)

    cache: dict = {}

    def score(combo):
        if combo not in cache:
            ours = png.decode(render_at(combo))
            cache[combo] = score_frames(
                ref, (opts.width, opts.height, bytes(ours.pixels)), WORLD_END)
        return cache[combo]

    def progress(i, total, row):
        if not opts.json:
            print(f"[{i:>3}/{total}] yaw {math.degrees(row['yaw'] - base_yaw):+6.2f}"
                  f"  pitch {math.degrees(row['pitch'] - base_pitch):+6.2f}"
                  f"  focal_y {row['focal_y']:7.2f}"
                  f"  corr {row['corr']:+.4f}  edges {row['edges']:.3f}",
                  flush=True)

    rows = fit(score, combos, progress, origin=(base_yaw, base_pitch, 0.0))
    for r in rows:
        r["yaw_delta_deg"] = math.degrees(r["yaw"] - base_yaw)
        r["pitch_delta_deg"] = math.degrees(r["pitch"] - base_pitch)
    best = rows[0]
    # Re-render the winner so --out can be inspected.
    if opts.out:
        with open(opts.out, "wb") as fh:
            fh.write(render_at((best["yaw"], best["pitch"], best["focal_y"],
                                best["eye_z"])))

    result = {
        "sidecar": opts.sidecar,
        "oracle": opts.oracle,
        "level": side["level"],
        "grid": {"yaw_deg": yaws, "pitch_deg": pitches, "focal_y": focals,
                 "eye_z": zs},
        "base": {"yaw_deg": math.degrees(base_yaw),
                 "pitch_deg": math.degrees(base_pitch)},
        "best": {**best,
                 "yaw_delta_deg": math.degrees(best["yaw"] - base_yaw),
                 "pitch_delta_deg": math.degrees(best["pitch"] - base_pitch)},
        "best_at_sidecar": next((r for r in rows
                                 if r["yaw"] == base_yaw and r["pitch"] == base_pitch
                                 and r["eye_z"] == 0.0), None),
        "top": rows[:opts.top],
        "frames": len(combos),
    }
    if opts.json:
        print(json.dumps(result, indent=2))
        return 0
    print(f"best: yaw {result['best']['yaw_delta_deg']:+.2f} deg, "
          f"pitch {result['best']['pitch_delta_deg']:+.2f} deg, "
          f"eye_z {best['eye_z']:+.1f}, focal_y {best['focal_y']:.2f}"
          f" -> corr {best['corr']:+.4f} "
          f"(edges {best['edges']:.3f})")
    base = result["best_at_sidecar"]
    if base is not None:
        gain = best["corr"] - base["corr"]
        print(f"sidecar camera: corr {base['corr']:+.4f}; gain {gain:+.4f} "
              f"over {result['frames']} frames")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
