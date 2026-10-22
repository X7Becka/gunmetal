#!/usr/bin/env python3
"""Compare an original-game frame (oracle) with a frame of our renderer.

The oracle is the original game running under DXWnd + dgVoodoo
(`docs/ORACLE.md`).  dgVoodoo may change filtering and colour slightly, so a
pixel-exact diff is not the goal: the tool reports structural agreement and
leaves the verdict to a human.

Metrics (computed on the full frame and on two bands: world / HUD):

  * ``corr`` — Pearson correlation of luma (1.0 = same picture);
  * ``mad``  — mean absolute difference, 0..1 (0 = identical);
  * ``edges``— Jaccard overlap of Sobel edges (geometry agreement);
  * ``hist`` — L1/2 distance of a 4x4x4 RGB histogram (colour mix).

The pass heuristic is deliberately conservative and **not a proof**; a
matching frame still has to be checked against the state (level, camera, save).

Examples::

    python3 tools/re/oracle_compare.py --selftest
    python3 tools/re/oracle_compare.py --oracle level0.png --ours out/level0.png
    python3 tools/re/oracle_compare.py --oracle level0.png --ours r.png \
        --crop 8,31,320,200 --json
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)

from gm import png  # noqa: E402


def load_rgb(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as fh:
        img = png.decode(fh.read())
    w, h = img.width, img.height
    px = img.pixels
    out = bytearray(w * h * 3)
    if img.mode == "P":
        pal = img.palette
        for i, idx in enumerate(px):
            out[3 * i] = pal[4 * idx]
            out[3 * i + 1] = pal[4 * idx + 1]
            out[3 * i + 2] = pal[4 * idx + 2]
    elif img.mode == "RGB":
        out[:] = px
    elif img.mode == "RGBA":
        for i in range(w * h):
            out[3 * i:3 * i + 3] = px[4 * i:4 * i + 3]
    elif img.mode == "L":
        for i, v in enumerate(px):
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    elif img.mode == "LA":
        for i in range(w * h):
            v = px[2 * i]
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    else:  # pragma: no cover - png.decode already rejects other modes
        raise ValueError(f"unsupported mode {img.mode}")
    return w, h, bytes(out)


def crop(img, box) -> tuple[int, int, bytes]:
    x, y, w, h = box
    sw, sh, px = img
    if x < 0 or y < 0 or x + w > sw or y + h > sh:
        raise SystemExit(f"crop {box} outside {sw}x{sh}")
    rows = [px[3 * ((y + r) * sw + x):3 * ((y + r) * sw + x + w)]
            for r in range(h)]
    return w, h, b"".join(rows)


def auto_crop_box(img, black: int = 8):
    """Bounding box of non-black pixels (dgVoodoo fullscreen pillars)."""
    w, h, px = img
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        base = 3 * (y * w)
        for x in range(w):
            o = base + 3 * x
            if px[o] > black or px[o + 1] > black or px[o + 2] > black:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return (0, 0, w, h)
    return (minx, miny, maxx - minx + 1, maxy - miny + 1)


def resize_area(img, tw: int, th: int) -> tuple[int, int, bytes]:
    """Area-average resample (exact 2x reduction for 640x400 -> 320x200)."""
    sw, sh, px = img
    if (sw, sh) == (tw, th):
        return img
    acc = [0] * (tw * th * 3)
    for ty in range(th):
        y0 = ty * sh // th
        y1 = max(y0 + 1, (ty + 1) * sh // th)
        for tx in range(tw):
            x0 = tx * sw // tw
            x1 = max(x0 + 1, (tx + 1) * sw // tw)
            r = g = b = n = 0
            for sy in range(y0, y1):
                base = 3 * (sy * sw + x0)
                for _sx in range(x0, x1):
                    r += px[base]
                    g += px[base + 1]
                    b += px[base + 2]
                    base += 3
                    n += 1
            o = 3 * (ty * tw + tx)
            acc[o] = r // n
            acc[o + 1] = g // n
            acc[o + 2] = b // n
    return tw, th, bytes(acc)


def luma(img, y0: int = 0, y1: int | None = None) -> list[float]:
    w, h, px = img
    y1 = h if y1 is None else y1
    out = []
    for y in range(y0, y1):
        base = 3 * (y * w)
        for x in range(w):
            o = base + 3 * x
            out.append(0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2])
    return out


def pearson(a: list[float], b: list[float]) -> float:
    n = len(a)
    if n == 0:
        return 0.0
    ma = sum(a) / n
    mb = sum(b) / n
    va = vb = cov = 0.0
    for x, y in zip(a, b):
        da = x - ma
        db = y - mb
        cov += da * db
        va += da * da
        vb += db * db
    if va <= 1e-9 or vb <= 1e-9:
        return 0.0
    return cov / (va ** 0.5 * vb ** 0.5)


def mad(a: list[float], b: list[float]) -> float:
    return sum(abs(x - y) for x, y in zip(a, b)) / (255.0 * max(len(a), 1))


def sobel_edges(img, y0: int = 0, y1: int | None = None,
                threshold: float = 32.0) -> list[bool]:
    w, h, px = img
    y1 = h if y1 is None else y1
    g = [[0.0] * w for _ in range(y1 - y0)]
    for y in range(y0, y1):
        base = 3 * (y * w)
        row = g[y - y0]
        for x in range(w):
            o = base + 3 * x
            row[x] = 0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2]
    mask = []
    for yy in range(1, len(g) - 1):
        for xx in range(1, w - 1):
            gx = (g[yy - 1][xx + 1] + 2 * g[yy][xx + 1] + g[yy + 1][xx + 1]
                  - g[yy - 1][xx - 1] - 2 * g[yy][xx - 1] - g[yy + 1][xx - 1])
            gy = (g[yy + 1][xx - 1] + 2 * g[yy + 1][xx] + g[yy + 1][xx + 1]
                  - g[yy - 1][xx - 1] - 2 * g[yy - 1][xx] - g[yy - 1][xx + 1])
            mask.append((gx * gx + gy * gy) ** 0.5 >= threshold)
    return mask


def jaccard(a: list[bool], b: list[bool]) -> float:
    inter = union = 0
    for x, y in zip(a, b):
        if x or y:
            union += 1
            if x and y:
                inter += 1
    return inter / union if union else 1.0


def histogram(img, y0: int = 0, y1: int | None = None) -> list[float]:
    w, h, px = img
    y1 = h if y1 is None else y1
    bins = [0] * 64
    n = 0
    for y in range(y0, y1):
        base = 3 * (y * w)
        for x in range(w):
            o = base + 3 * x
            bins[(px[o] >> 6) * 16 + (px[o + 1] >> 6) * 4 + (px[o + 2] >> 6)] += 1
            n += 1
    return [v / n for v in bins]


def hist_distance(a: list[float], b: list[float]) -> float:
    return sum(abs(x - y) for x, y in zip(a, b)) / 2.0


def band(img, start: float, end: float) -> tuple[int, int]:
    w, h, _px = img
    return int(h * start), max(int(h * start) + 1, int(h * end))


def metrics(oracle, ours, world_end: float = 0.7) -> dict:
    h = oracle[1]
    ranges = {"frame": (0, h),
              "world": band(oracle, 0.0, world_end),
              "hud": band(oracle, world_end, 1.0)}
    out = {}
    for name, (y0, y1) in ranges.items():
        a = luma(oracle, y0, y1)
        b = luma(ours, y0, y1)
        out[name] = {
            "corr": round(pearson(a, b), 4),
            "mad": round(mad(a, b), 4),
            "edges": round(jaccard(sobel_edges(oracle, y0, y1),
                                   sobel_edges(ours, y0, y1)), 4),
            "hist": round(hist_distance(histogram(oracle, y0, y1),
                                        histogram(ours, y0, y1)), 4),
        }
    return out


def selftest() -> int:
    def make(kind: str) -> tuple[int, int, bytes]:
        w = h = 64
        px = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                o = 3 * (y * w + x)
                if kind == "gradient":
                    v = (x * 4 + y * 2) % 256
                    px[o:o + 3] = bytes((v, (v * 2) % 256, 255 - v))
                elif kind == "noise":
                    v = (x * 73 + y * 151 + (x * y) % 251) % 256
                    px[o:o + 3] = bytes((v, (v * 7) % 256, (v * 13) % 256))
                elif kind == "shift":
                    xs = (x + 7) % w
                    v = (xs * 4 + y * 2) % 256
                    px[o:o + 3] = bytes((v, (v * 2) % 256, 255 - v))
        return w, h, bytes(px)

    a, b, c, d = make("gradient"), make("gradient"), make("shift"), make("noise")
    m_same = metrics(a, b)["frame"]
    m_shift = metrics(a, c)["frame"]
    m_noise = metrics(a, d)["frame"]
    checks = [
        ("same corr is 1", abs(m_same["corr"] - 1.0) < 1e-9),
        ("same mad is 0", m_same["mad"] == 0.0),
        ("same edges is 1", m_same["edges"] == 1.0),
        ("shift lowers corr", m_shift["corr"] < 0.999),
        ("noise corr is low", abs(m_noise["corr"]) < 0.5),
        ("noise edges are not perfect", m_noise["edges"] < 1.0),
    ]
    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    print(f"selftest: {len(checks) - len(failed)}/{len(checks)}")
    return 0 if not failed else 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Oracle vs our-render frame compare.")
    ap.add_argument("--oracle", help="captured original-game frame (PNG)")
    ap.add_argument("--ours", help="our rendered frame (PNG)")
    ap.add_argument("--crop", help="oracle crop x,y,w,h (window borders)")
    ap.add_argument("--auto-crop", dest="auto_crop", action="store_true",
                    help="crop the oracle to the non-black picture rectangle")
    ap.add_argument("--black", type=int, default=8,
                    help="auto-crop threshold 0..255 (default 8)")
    ap.add_argument("--size", default="320x200", help="compare on WxH (default)")
    ap.add_argument("--hud", type=float, default=0.7,
                    help="share of height that is world (default 0.7)")
    ap.add_argument("--min-corr", type=float, default=0.9)
    ap.add_argument("--min-edges", type=float, default=0.5)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    opts = ap.parse_args(argv)

    if opts.selftest:
        return selftest()
    if not opts.oracle or not opts.ours:
        ap.error("--oracle and --ours are required (or --selftest)")

    tw, th = (int(v) for v in opts.size.lower().split("x"))
    oracle = load_rgb(opts.oracle)
    crop_box = None
    if opts.crop:
        crop_box = tuple(int(v) for v in opts.crop.split(","))
    elif opts.auto_crop:
        crop_box = auto_crop_box(oracle, opts.black)
        print(f"auto-crop: {crop_box[0]},{crop_box[1]},"
              f"{crop_box[2]},{crop_box[3]}", file=sys.stderr)
    if crop_box:
        oracle = crop(oracle, crop_box)
    ours = load_rgb(opts.ours)
    oracle = resize_area(oracle, tw, th)
    ours = resize_area(ours, tw, th)
    result = {"oracle": opts.oracle, "ours": opts.ours,
              "crop": crop_box, "size": [tw, th],
              "world_share": opts.hud,
              "metrics": metrics(oracle, ours, opts.hud)}
    world = result["metrics"]["world"]
    passed = (world["corr"] >= opts.min_corr and
              world["edges"] >= opts.min_edges)
    result["heuristic_pass"] = passed
    if opts.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        for band_name, m in result["metrics"].items():
            print(f"{band_name:5s} corr={m['corr']:+.3f} mad={m['mad']:.3f} "
                  f"edges={m['edges']:.3f} hist={m['hist']:.3f}")
        print("heuristic:", "match" if passed else "differs",
              "(not a proof — check level/camera/save state)")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
