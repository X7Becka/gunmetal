#!/usr/bin/env python3
"""Noise- and shift-tolerant comparison of an oracle frame with our render.

Why this tool exists
--------------------
``tools/re/oracle_compare.py`` reports a global Pearson ``corr`` of luma, the
Jaccard overlap of Sobel edges, ``mad`` and a global histogram distance.  On
the captured frames (``docs/ORACLE.md``) that combination **rewards texture
noise**: the frame whose floor is covered by cyan speckle (``uv="fit"`` --
polygons without a texture stretched and shaded) scores ``corr`` 0.20, while
the visually correct frame with the fine engine grid (``uv="texel"`` plus
skipping untextured polygons) scores 0.11 -- even though ``mad`` and the global
``hist`` improve.  The decision between the UV modes therefore cannot be made
with ``oracle_compare``; see ``docs/FRAME_METRIC.md`` for the numbers.

The mechanism is structural.  Pearson is one global sum, so a bright speckle
field that merely sits where the reference is bright (the lit floor, the ramp)
earns a positive covariance without reproducing a single texel.  Per-block
comparison removes that mechanism; comparing a low-pass (mip) image stops
one-pixel noise from contributing at all.

Metrics (each 0..1, 1 = identical)
----------------------------------
``edge_f1``
    Shift-tolerant Sobel-edge F1.  Both frames are low-passed (binomial 7-tap,
    sigma ~ 1.2 px; the capture is area-downsampled from 1920x1080 and is
    softer than our nearest-neighbour render), Sobel magnitudes are thresholded
    at ``max(24, P90)`` of the band -- the same edge *density* on both sides,
    so sharpness does not decide the score -- and an edge of one frame counts
    as matched when an edge of the other lies within ``d`` pixels (Euclidean
    disk, ``--edge-tol``, default 2).  F1 is the harmonic mean of precision
    (our edges present in the oracle) and recall (oracle edges reproduced).
    The tolerance makes a 1-3 px shift harmless; density matching stops a
    noisy frame from winning by simply having more edges.
``colour``
    1 - chi^2 of the *worst quarter* of the 8 x 8 px blocks, where the distance
    of a block is the symmetric chi^2 of its colour histogram over a 32-bin
    colormap: 8 luma bands x 4 chroma classes (neutral / warm / green / cool).
    Block-local histograms cannot be paid for by dark pixels elsewhere, and a
    *chroma* class cannot be paid for by luma.  Both frames are first scaled by
    one global gain fitted on luma: the absolute brightness of our render is a
    separate, known defect (docs/LIGHTMAP_AUDIT.md) and would otherwise dominate
    every comparison.  Symmetric chi^2 is used instead of L1 because the signed
    brightness errors of the two variants cancel in L1 (measured: block-mean L1
    is blind to the cyan plate).  The worst quarter, not the mean, is what an
    artifact lives in: a plate covering a fifth of the frame moves the mean
    chi^2 by 0.03 and the worst quarter by 0.19.
``ncc``
    Mean local normalized cross-correlation of 16 x 16 blocks on a 2 x 2
    (mip 1) reduced luma, negative correlations clamped to 0.  Every block is
    compared after subtracting its *own* mean, so the "bright here, bright
    there" agreement that fools the global ``corr`` contributes nothing, and
    the 2 x 2 average suppresses one-pixel speckle.  A block that is flat in
    one frame but not in the other scores 0 (exactly: a noise texture on a
    smooth surface); blocks flat in both are skipped.
``score``
    ``0.30*edge_f1 + 0.45*colour + 0.25*ncc``, times a polarity gate: if
    ``colour < 0.10`` or ``ncc < 0.02`` the frame shares no colour and/or no
    local structure with the reference, and the score is 0 (this is what makes
    a brightness inversion, which leaves the Sobel map untouched, score ~0
    instead of inheriting 0.30).

    The weights are calibrated, not guessed: the ranking of the real variants
    (docs/FRAME_METRIC.md, "калибровка") is independent of how the remaining
    weight is split between ``edge_f1`` and ``ncc`` exactly when colour carries
    >= 0.45, so 0.45 is the smallest colour weight that makes the verdict
    robust; the structural terms keep 0.30/0.25 as their measured reliability
    allows.  Below that threshold the edge term, which on these frames sits
    almost at its cross-frame chance level, decides -- and it prefers the noisy
    variant.

``match`` is ``score >= 0.60``.  Measured (`--self-test`, `--diagnose`): the
positives (identical, shifted 1-2 px, +8 % gain) score 0.991-1.000, the
negatives (brightness inversion, cyan speckle plate, a different frame) 0.000,
so the threshold sits in the middle with >= 0.34 margin on both sides.  The
verdict is about the *picture*: a real frame that still lacks textures/lighting
stays below the threshold, which is the honest answer (docs/ORACLE.md:
``win1..win3`` do not reproduce yet).

Reliability on the captured frames
----------------------------------
The alignment terms sit close to their cross-frame chance level on
``win2``/``win3`` -- two unrelated frames score edge F1 0.18-0.22 and NCC
0.16-0.17, while the real pairs score 0.24-0.34 and 0.13-0.22 -- because the
camera is *fitted*, not measured (docs/ORACLE.md, missing ``turret_pitch``).
Only the colour term is clearly above chance (0.105-0.296 against 0.085).  That
is why the calibration is a *ranking of variants that share one camera* and
why the report prints all components instead of only the composite.

CLI
---
    python3 tools/re/frame_metric.py --self-test
    python3 tools/re/frame_metric.py --oracle win2.png --ours r.png --bands
    python3 tools/re/frame_metric.py --oracle win2.png --ours r.png --json --shift 3
    python3 tools/re/frame_metric.py --diagnose   # benign vs artifact response
    python3 tools/re/frame_metric.py --calibrate  # win2/win3 x four UV modes
    python3 tools/re/frame_metric.py --calibrate --json | tail -20

Captures are looked up in ``--oracle-dir`` (default: ``out/oracle`` of this
checkout, then ``/home/nooverload/gunmetal-re/out/oracle``).  Rendering only
reads the installation (``gm.render`` opens the level, the lightmaps and the
texture pool read-only).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)

import oracle_compare as oc  # noqa: E402  (reused, never modified)

#: comparison grid of the original game at 320x200 (docs/ORACLE.md)
DEFAULT_SIZE = (320, 200)
#: black window frame of a capture, in source pixels (oracle_render.py)
ORACLE_BORDER_PX = 1
#: share of the frame height that is world; the cockpit starts below it
WORLD_END = 0.70
#: edge tolerance (pixels), edge density quantile, absolute gradient floor
EDGE_TOL = 2
EDGE_QUANTILE = 0.90
EDGE_FLOOR = 24.0
#: blocks: colour at full resolution, correlation at mip 1
COLOUR_BLOCK = 8
COLOUR_LUMA_BANDS = 8
NCC_BLOCK = 16
NCC_MIP = 1
#: global shift search, pixels each way (0 disables it)
SHIFT = 2
#: composite: weights (sum 1), polarity gate, match threshold
WEIGHTS = {"edge": 0.30, "colour": 0.45, "ncc": 0.25}
GATE_COLOUR = 0.10
GATE_NCC = 0.02
#: a frame whose block NCC is (on average) negative is anti-correlated
GATE_SIGNED = -0.02
MATCH_THRESHOLD = 0.60
#: default oracle capture directories (first existing wins)
ORACLE_DIRS = (os.path.join(ROOT, "out", "oracle"),
               "/home/nooverload/gunmetal-re/out/oracle")
#: camera pitch of the calibration renders, measured in docs/ORACLE.md
CALIBRATION = {"win2": 8.0, "win3": 14.0}
#: the four render variants of the UV/polygon question (docs/TEXUV.md)
VARIANTS = (("fit", "fit", False),
            ("texel", "texel", False),
            ("fit+skip", "fit", True),
            ("texel+skip", "texel", True))
GAME_DEFAULT = "/mnt/d/games/Gunmetal 3dfx"


# ---------------------------------------------------------------------------
# images
# ---------------------------------------------------------------------------

def load_rgb(path: str) -> tuple[int, int, bytes]:
    """PNG -> 8-bit RGB, reusing the loader of oracle_compare."""
    return oc.load_rgb(path)


def prepare_oracle(path: str, size=DEFAULT_SIZE, border: int | None = None,
                   crop_box=None):
    """Capture -> comparison grid: optional crop, then area-downsample.

    A capture is a 1920x1080 grab of a 320x200 image plus a 1-pixel black
    window frame (``oracle_render.py``); area-averaging keeps thin geometry.  A
    PNG that already is the target size is used as is (unless ``crop_box`` or
    ``border`` is given explicitly).
    """
    img = load_rgb(path)
    if crop_box is not None:
        img = oc.crop(img, tuple(crop_box))
        border = 0
    if (img[0], img[1]) == tuple(size) and not border:
        return img
    if border is None:
        border = ORACLE_BORDER_PX
    if border > 0:
        sw, sh, _px = img
        img = oc.crop(img, (border, border, sw - 2 * border, sh - 2 * border))
    if (img[0], img[1]) == tuple(size):
        return img
    return oc.resize_area(img, size[0], size[1])


def fit_to_size(img, size=DEFAULT_SIZE):
    if (img[0], img[1]) == tuple(size):
        return img
    return oc.resize_area(img, size[0], size[1])


def luma(img) -> bytearray:
    """Rec.601 luma of every pixel (0..255)."""
    w, h, px = img
    out = bytearray(w * h)
    for i in range(w * h):
        o = 3 * i
        out[i] = (299 * px[o] + 587 * px[o + 1] + 114 * px[o + 2]) // 1000
    return out


def gain_luma(img, gain: float):
    """The frame with every channel multiplied by ``gain`` (clipped)."""
    if abs(gain - 1.0) < 1e-6:
        return img
    w, h, px = img
    lut = bytes(min(255, int(v * gain)) for v in range(256))
    return (w, h, bytes(px.translate(lut)))


def fit_gain(o, u, y0: int, y1: int) -> float:
    """One global luma gain g with sum(luma_o) = g * sum(luma_u)."""
    _w, _h, po = o
    _w2, _h2, pu = u
    w = o[0]
    so = su = 0
    for y in range(y0, y1):
        row = y * w
        for x in range(w):
            i = 3 * (row + x)
            so += 299 * po[i] + 587 * po[i + 1] + 114 * po[i + 2]
            su += 299 * pu[i] + 587 * pu[i + 1] + 114 * pu[i + 2]
    return so / su if su else 1.0


def shift_rows(img, dx: int, dy: int, y0: int, y1: int):
    """Translate an image by (dx, dy), clamping at the border.

    Only the rows ``y0..y1`` are touched; the metrics look at that band, and a
    translation must not pull in pixels of the other band.
    """
    w, h, px = img
    if dx == 0 and dy == 0:
        return img
    out = bytearray(px)
    stride = 3 * w
    for y in range(y0, y1):
        sy = min(h - 1, max(0, y + dy))
        src = sy * stride
        dst = y * stride
        if dx == 0:
            out[dst:dst + stride] = px[src:src + stride]
            continue
        for x in range(w):
            sx = min(w - 1, max(0, x + dx))
            out[dst + 3 * x:dst + 3 * x + 3] = px[src + 3 * sx:src + 3 * sx + 3]
    return (w, h, bytes(out))


def bands(height: int, world_end: float = WORLD_END) -> dict:
    end = max(1, min(height, int(round(height * world_end))))
    return {"frame": (0, height), "world": (0, end), "hud": (end, height)}


# ---------------------------------------------------------------------------
# (a) shift-tolerant Sobel edge F1
# ---------------------------------------------------------------------------

#: binomial kernel B6 = [1 6 15 20 15 6 1]/64, sigma ~ 1.2 px per axis
_KERNEL = (1, 6, 15, 20, 15, 6, 1)


def gauss(l: bytearray, w: int, h: int, passes: int = 1) -> bytearray:
    """Separable binomial low-pass."""
    for _ in range(passes):
        tmp = [0] * (w * h)
        for y in range(h):
            b = y * w
            for x in range(w):
                s = 0
                for k, kk in enumerate(_KERNEL):
                    s += kk * l[b + min(w - 1, max(0, x + k - 3))]
                tmp[b + x] = s >> 6
        out = bytearray(w * h)
        for y in range(h):
            for x in range(w):
                s = 0
                for k, kk in enumerate(_KERNEL):
                    s += kk * tmp[min(h - 1, max(0, y + k - 3)) * w + x]
                out[y * w + x] = s >> 6
        l = out
    return l


def sobel(l: bytearray, w: int, h: int) -> list[float]:
    """Sobel gradient magnitude, zero on the 1-pixel border."""
    mag = [0.0] * (w * h)
    for y in range(1, h - 1):
        b = y * w
        up = b - w
        dn = b + w
        for x in range(1, w - 1):
            gx = (l[up + x + 1] + 2 * l[b + x + 1] + l[dn + x + 1]
                  - l[up + x - 1] - 2 * l[b + x - 1] - l[dn + x - 1])
            gy = (l[dn + x - 1] + 2 * l[dn + x] + l[dn + x + 1]
                  - l[up + x - 1] - 2 * l[up + x] - l[up + x + 1])
            mag[b + x] = math.hypot(gx, gy)
    return mag


def quantile(values, q: float) -> float:
    nz = sorted(v for v in values if v > 0.0)
    if not nz:
        return 0.0
    return nz[min(len(nz) - 1, int(q * len(nz)))]


def _disk(d: int) -> list[tuple[int, int]]:
    return [(dx, dy) for dx in range(-d, d + 1) for dy in range(-d, d + 1)
            if dx * dx + dy * dy <= d * d]


def _matched_fraction(a: bytearray, b: bytearray, w: int,
                      y0: int, y1: int, d: int) -> tuple[float | None, int]:
    """Share of ``a``'s edge pixels that have an edge of ``b`` within ``d``."""
    total = sum(a[y * w:(y + 1) * w].count(1) for y in range(y0, y1))
    if total == 0:
        return None, 0
    off = _disk(d)
    hit = 0
    for y in range(y0, y1):
        row = y * w
        for x in range(w):
            if not a[row + x]:
                continue
            for dx, dy in off:
                xx = x + dx
                yy = y + dy
                if 0 <= xx < w and y0 <= yy < y1 and b[yy * w + xx]:
                    hit += 1
                    break
    return hit / total, total


def edge_f1(oracle_luma, ours_luma, w, h, y0, y1,
            tol: int = EDGE_TOL, quantile_p: float = EDGE_QUANTILE,
            floor: float = EDGE_FLOOR) -> dict:
    """Sobel-edge F1 with a pixel tolerance on both frames (see module doc)."""
    return edge_f1_pre(edge_reference(oracle_luma, w, h, y0, y1, quantile_p,
                                      floor),
                       ours_luma, w, h, y0, y1, tol)


def edge_reference(oracle_luma, w, h, y0, y1, quantile_p: float = EDGE_QUANTILE,
                   floor: float = EDGE_FLOOR) -> dict:
    """Blur, Sobel and threshold the reference once (it is reused per shift)."""
    blur = gauss(oracle_luma, w, h)
    mag = sobel(blur, w, h)
    thr = max(floor, quantile([v for y in range(y0, y1)
                               for v in mag[y * w:(y + 1) * w]], quantile_p))
    mask = bytearray(w * h)
    for y in range(y0, y1):
        b = y * w
        for x in range(w):
            if mag[b + x] >= thr:
                mask[b + x] = 1
    return {"mask": mask, "threshold": thr}


def edge_f1_pre(ref: dict, ours_luma, w, h, y0, y1, tol: int = EDGE_TOL,
                quantile_p: float = EDGE_QUANTILE,
                floor: float = EDGE_FLOOR) -> dict:
    """Edge F1 against a pre-thresholded reference."""
    lu = gauss(ours_luma, w, h)
    mu = sobel(lu, w, h)
    tu = max(floor, quantile([v for y in range(y0, y1)
                              for v in mu[y * w:(y + 1) * w]], quantile_p))
    eu = bytearray(w * h)
    for y in range(y0, y1):
        b = y * w
        for x in range(w):
            if mu[b + x] >= tu:
                eu[b + x] = 1
    eo = ref["mask"]
    precision, n_ours = _matched_fraction(eu, eo, w, y0, y1, tol)
    recall, n_oracle = _matched_fraction(eo, eu, w, y0, y1, tol)
    if precision is None and recall is None:
        f1 = 1.0                      # both bands empty: nothing to miss
    elif not precision or not recall or precision + recall == 0:
        f1 = 0.0
    else:
        f1 = 2 * precision * recall / (precision + recall)
    return {"f1": f1,
            "precision": precision if precision is not None else 0.0,
            "recall": recall if recall is not None else 0.0,
            "edges_ours": n_ours, "edges_oracle": n_oracle,
            "threshold": [round(ref["threshold"], 2), round(tu, 2)],
            "tol": tol}


# ---------------------------------------------------------------------------
# (b) block colour distance over a 32-bin colormap
# ---------------------------------------------------------------------------

def colour_class(r: int, g: int, b: int) -> int:
    """luma band (0..7) x chroma class (0 neutral, 1 warm, 2 green, 3 cool)."""
    l = (299 * r + 587 * g + 114 * b) // 1000
    band = min(COLOUR_LUMA_BANDS - 1, l * COLOUR_LUMA_BANDS // 256)
    mx = max(r, g, b)
    if mx < 12 or (mx - min(r, g, b)) * 4 < mx:
        chroma = 0
    elif b >= g and b >= r:
        chroma = 3
    elif g >= r:
        chroma = 2
    else:
        chroma = 1
    return band * 4 + chroma


def block_colormap(img, w, h, y0, y1, block: int = COLOUR_BLOCK):
    """Normalised 32-bin colormap histogram per block of the band."""
    nbin = COLOUR_LUMA_BANDS * 4
    bw = (w + block - 1) // block
    bh = (y1 - y0 + block - 1) // block
    hist = [[0] * nbin for _ in range(bw * bh)]
    _w, _h, px = img
    for y in range(y0, y1):
        by = (y - y0) // block
        row = y * w
        for x in range(w):
            o = 3 * (row + x)
            hist[by * bw + x // block][colour_class(px[o], px[o + 1],
                                                   px[o + 2])] += 1
    for blk in hist:
        n = sum(blk)
        for k in range(nbin):
            blk[k] /= n
    return hist, bw, bh


def colour_distance(oracle, ours, w, h, y0, y1,
                    block: int = COLOUR_BLOCK) -> dict:
    """Colour score of two frames (analyses the reference each call)."""
    ho, bw, bh = block_colormap(oracle, w, h, y0, y1, block)
    return colour_distance_pre((ho, bw, bh), oracle, ours, w, h, y0, y1, block)


def colour_distance_pre(ref, oracle, ours, w, h, y0, y1,
                        block: int = COLOUR_BLOCK) -> dict:
    """Mean of the *worst quarter* of block chi^2, after one global gain fit.

    Artifacts are local and gross: averaging chi^2 over every block dilutes a
    cyan plate covering a fifth of the frame (measured: the mean moves by
    0.03 while the worst quarter moves by 0.19).  The worst quarter is what a
    viewer reacts to, and it also keeps the score honest across *different*
    frames (two unrelated interiors get ~0, not 0.85).
    """
    gain = fit_gain(oracle, ours, y0, y1)
    ours = gain_luma(ours, gain)
    ho, bw, bh = ref
    hu, _bw, _bh = block_colormap(ours, w, h, y0, y1, block)
    chis = []
    for a, b in zip(ho, hu):
        c = 0.0
        for x, y in zip(a, b):
            s = x + y
            if s > 0.0:
                c += (x - y) ** 2 / s
        chis.append(0.5 * c)
    chis.sort()
    mean = sum(chis) / len(chis)
    tail = chis[int(len(chis) * 0.75):]      # the worst quarter of the blocks
    worst = sum(tail) / len(tail)
    return {"score": max(0.0, 1.0 - worst), "chi2": mean,
            "chi2_worst_quartile": worst,
            "blocks": len(chis), "block": block,
            "bins": COLOUR_LUMA_BANDS * 4, "gain": round(gain, 4)}


# ---------------------------------------------------------------------------
# (c) block NCC on a mip level (noise suppression)
# ---------------------------------------------------------------------------

def mip_luma(lum: bytearray, w: int, h: int, y0: int, y1: int,
             mip: int = NCC_MIP) -> tuple[list[float], int, int]:
    """Box-downsample luma by ``2**mip`` over the band."""
    f = 1 << mip
    tw = w // f
    th = (y1 - y0) // f
    out = [0.0] * (tw * th)
    inv = 1.0 / (f * f)
    for y in range(th):
        dy = y0 + y * f
        for x in range(tw):
            s = 0
            for yy in range(dy, dy + f):
                b = yy * w + x * f
                s += sum(lum[b:b + f])
            out[y * tw + x] = s * inv
    return out, tw, th


def block_ncc(oa, ob, tw, th, block: int = NCC_BLOCK) -> dict:
    """Mean positive block NCC; blocks flat in both frames are skipped."""
    scores = []
    signed = []
    flat_noise = 0
    for by in range(0, th - block + 1, block):
        for bx in range(0, tw - block + 1, block):
            n = 0
            sa = sb = 0.0
            for y in range(by, by + block):
                r = y * tw + bx
                sa += sum(oa[r:r + block])
                sb += sum(ob[r:r + block])
                n += block
            ma = sa / n
            mb = sb / n
            va = vb = cov = 0.0
            for y in range(by, by + block):
                r = y * tw + bx
                for x in range(r, r + block):
                    da = oa[x] - ma
                    db = ob[x] - mb
                    va += da * da
                    vb += db * db
                    cov += da * db
            va /= n
            vb /= n
            cov /= n
            if va < 1.0 and vb < 1.0:
                continue              # flat in both: no structure to compare
            if va < 1.0 or vb < 1.0:
                flat_noise += 1       # noise where the other frame is flat
                scores.append(0.0)
                signed.append(0.0)
                continue
            r = cov / math.sqrt(va * vb)
            scores.append(max(0.0, r))
            signed.append(r)
    if not scores:
        return {"score": 1.0, "signed": 1.0, "blocks": 0, "noise_blocks": 0}
    return {"score": sum(scores) / len(scores),
            "signed": sum(signed) / len(signed),
            "blocks": len(scores), "noise_blocks": flat_noise}


# ---------------------------------------------------------------------------
# composite
# ---------------------------------------------------------------------------

def reference_parts(oracle, w, h, y0, y1) -> dict:
    """Everything the metrics need from the reference frame (once per band)."""
    l = luma(oracle)
    ref = {"size": (w, h), "rows": (y0, y1), "luma": l,
           "edges": edge_reference(l, w, h, y0, y1),
           "colour": block_colormap(oracle, w, h, y0, y1)[:3]}
    ref["mip"] = mip_luma(l, w, h, y0, y1)
    return ref


def components(oracle, ours, w, h, y0, y1, tol: int = EDGE_TOL) -> dict:
    """All three metric blocks for one pair of frames (reference analysed)."""
    return components_pre(reference_parts(oracle, w, h, y0, y1), oracle, ours,
                          tol)


def components_pre(ref: dict, oracle, ours, tol: int = EDGE_TOL) -> dict:
    """All three metric blocks, reusing a precomputed reference."""
    w, h = ref["size"]
    y0, y1 = ref["rows"]
    ul = luma(ours)
    e = edge_f1_pre(ref["edges"], ul, w, h, y0, y1, tol)
    c = colour_distance_pre(ref["colour"], oracle, ours, w, h, y0, y1)
    ua, tw, th = mip_luma(ul, w, h, y0, y1)
    n = block_ncc(ref["mip"][0], ua, tw, th)
    return {"edge_f1": e, "colour": c, "ncc": n}


def composite(parts: dict) -> float:
    """Weighted mean of the three scores, gated on colour and local structure."""
    e = max(0.0, min(1.0, parts["edge_f1"]["f1"]))
    c = max(0.0, min(1.0, parts["colour"]["score"]))
    n = max(0.0, min(1.0, parts["ncc"]["score"]))
    if c < GATE_COLOUR or n < GATE_NCC or parts["ncc"]["signed"] < GATE_SIGNED:
        return 0.0        # no common colour / no common (or inverted) structure
    return (WEIGHTS["edge"] * e + WEIGHTS["colour"] * c + WEIGHTS["ncc"] * n)


def compare_images(oracle, ours, size=DEFAULT_SIZE, shift: int = SHIFT,
                   tol: int = EDGE_TOL, band: str = "world",
                   world_end: float = WORLD_END) -> dict:
    """All metrics for one band, at the best global shift <= ``shift`` px.

    A small translation is not a picture difference (the oracle state comes
    from a live process, docs/ORACLE.md), so the numbers reported are those of
    the shift that maximises the composite.  The shift is printed; a large one
    means the camera is off, not that the frame matches.
    """
    w, h = size
    y0, y1 = bands(h, world_end)[band]
    best = None
    steps = 0
    ref = reference_parts(oracle, w, h, y0, y1)
    for dy in range(-shift, shift + 1):
        for dx in range(-shift, shift + 1):
            steps += 1
            moved = shift_rows(ours, dx, dy, y0, y1)
            parts = components_pre(ref, oracle, moved, tol)
            score = composite(parts)
            if best is None or score > best[0] + 1e-12:
                best = (score, dx, dy, parts)
    score, dx, dy, parts = best
    return {"score": round(score, 4), "shift": [dx, dy], "shift_tried": steps,
            "band": band, "band_rows": [y0, y1], "size": [w, h],
            "match": score >= MATCH_THRESHOLD,
            "threshold": MATCH_THRESHOLD,
            "weights": dict(WEIGHTS),
            "metrics": parts}


def compare_files(oracle_path: str, ours_path: str, size=DEFAULT_SIZE,
                  shift: int = SHIFT, tol: int = EDGE_TOL,
                  band: str = "world", all_bands: bool = False,
                  border: int | None = None,
                  world_end: float = WORLD_END,
                  with_old: bool = False, crop_box=None) -> dict:
    """Compare two PNG files; ``--bands`` adds frame/world/hud breakdowns."""
    oracle = prepare_oracle(oracle_path, size, border, crop_box)
    ours = fit_to_size(load_rgb(ours_path), size)
    wanted = ("frame", "world", "hud") if all_bands else (band,)
    out = {"oracle": oracle_path, "ours": ours_path,
           "size": list(size), "world_end": world_end,
           "crop": list(crop_box) if crop_box else None, "bands": {}}
    for name in wanted:
        out["bands"][name] = compare_images(oracle, ours, size, shift, tol,
                                            name, world_end)
    if not all_bands:
        res = out["bands"][band]
        out["band"] = band
        out["band_rows"] = res["band_rows"]
        for k in ("score", "shift", "match", "threshold", "weights", "metrics"):
            out[k] = res[k]
    if with_old:
        out["old"] = {name: oc.metrics(oracle, ours, world_end=world_end)[name]
                      for name in wanted}
    return out


# ---------------------------------------------------------------------------
# calibration on the captured frames (renders four UV variants)
# ---------------------------------------------------------------------------

def find_oracle_dir(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    for cand in ORACLE_DIRS:
        if os.path.isdir(cand):
            return cand
    return ORACLE_DIRS[-1]


def _pitch_camera(cam: dict, pitch_deg: float) -> dict:
    """Sidecar camera with the fitted pitch (docs/ORACLE.md, turret_pitch)."""
    import oracle_render as orend
    cam = dict(cam)
    cam["pitch"] = math.radians(pitch_deg)
    cam.pop("camera_pitch", None)
    return orend.camera_frame(cam)


def calibrate(oracle_dir: str, game: str, shift: int = SHIFT,
              tol: int = EDGE_TOL, wide: bool = False) -> dict:
    """Render the four UV variants of win2/win3 and score them.

    ``fit``        one copy of the texture stretched over the whole polygon
    ``texel``      the engine texel projection (docs/TEXUV.md)
    ``fit+skip``   ``fit`` without polygons that have no texture
    ``texel+skip`` ``texel`` without those polygons
    """
    import tempfile
    import oracle_render as orend
    from gm import render, sbw

    out = {"oracle_dir": oracle_dir, "game": game, "size": list(DEFAULT_SIZE),
           "shift": shift, "edge_tol": tol, "frames": {}}
    tmp = tempfile.mkdtemp(prefix="frame-metric-")
    for name, pitch_deg in CALIBRATION.items():
        sidecar = os.path.join(oracle_dir, name + ".json")
        oracle_png = os.path.join(oracle_dir, name + ".png")
        if not (os.path.exists(sidecar) and os.path.exists(oracle_png)):
            out["frames"][name] = {"error": "missing sidecar or PNG"}
            continue
        side = orend.load_sidecar(sidecar)
        frame = _pitch_camera(side["camera"], pitch_deg)
        sbw_path, sbc_path, meshes = orend.build_scene(game, side["level"])
        pool = render.TexturePool(game)
        oracle = prepare_oracle(oracle_png)
        rows = {}
        for tag, uv, skip in VARIANTS:
            data = render.render(
                meshes, width=DEFAULT_SIZE[0], height=DEFAULT_SIZE[1],
                pool=pool, pos=frame["eye"], look=frame["look"],
                focal=orend.default_focal(DEFAULT_SIZE[0]),
                lightmap=True, materials=True, ambient=0.0, chromakey=False,
                uv=uv, skip_untextured=skip)
            path = os.path.join(tmp, "%s_%s%s.png"
                                % (name, uv, "_skip" if skip else ""))
            with open(path, "wb") as fh:
                fh.write(data)
            ours = load_rgb(path)
            res = compare_images(oracle, ours, DEFAULT_SIZE, shift, tol)
            old = oc.metrics(oracle, ours)["world"]
            em = res["metrics"]["edge_f1"]
            cm = res["metrics"]["colour"]
            rows[tag] = {
                "score": res["score"], "match": res["match"],
                "shift": res["shift"],
                "edge_f1": round(em["f1"], 4),
                "edge_precision": round(em["precision"], 4),
                "edge_recall": round(em["recall"], 4),
                "edges_ours": em["edges_ours"],
                "edges_oracle": em["edges_oracle"],
                "colour": round(cm["score"], 4),
                "chi2_mean": round(cm["chi2"], 4),
                "chi2_worst": round(cm["chi2_worst_quartile"], 4),
                "gain": cm["gain"],
                "ncc": round(res["metrics"]["ncc"]["score"], 4),
                "ncc_signed": round(res["metrics"]["ncc"]["signed"], 4),
                "old": {k: old[k] for k in ("corr", "mad", "edges", "hist")},
                "png": path,
            }
        base = rows["fit"]["score"]
        for tag in rows:
            rows[tag]["vs_fit"] = round(rows[tag]["score"] - base, 4)
            if wide:
                full = compare_files(oracle_png, rows[tag]["png"],
                                     DEFAULT_SIZE, shift, tol, "world", True)
                rows[tag]["bands"] = {b: full["bands"][b]["score"]
                                      for b in full["bands"]}
        out["frames"][name] = {"pitch_deg": pitch_deg, "level": side["level"],
                               "variants": rows}
    out["tmp"] = tmp
    return out


def print_calibration(rep: dict) -> None:
    print(f"oracle dir {rep['oracle_dir']}")
    for name, fr in rep["frames"].items():
        if "error" in fr:
            print(f"{name}: {fr['error']}")
            continue
        print(f"\n{name}: level {fr['level']}, fitted pitch "
              f"+{fr['pitch_deg']:.0f} deg (docs/ORACLE.md), world band")
        print(f"  {'variant':11s} {'score':>6s} {'vs fit':>7s} {'match':>5s} "
              f"{'edge_f1':>7s} {'colour':>6s} {'ncc':>5s} | "
              f"{'old corr':>8s} {'hist':>5s} {'mad':>5s}")
        for tag, r in fr["variants"].items():
            old = r["old"]
            print(f"  {tag:11s} {r['score']:6.3f} {r['vs_fit']:+7.3f} "
                  f"{'yes' if r['match'] else 'no':>5s} "
                  f"{r['edge_f1']:7.3f} {r['colour']:6.3f} {r['ncc']:5.3f} | "
                  f"{old['corr']:+8.3f} {old['hist']:5.3f} {old['mad']:5.3f}")
        best = max(fr["variants"], key=lambda t: fr["variants"][t]["score"])
        print(f"  best by score: {best} "
              f"({fr['variants'][best]['score']:.3f})")


# ---------------------------------------------------------------------------
# diagnostics: what a *benign* and an *artifact* change does to each term
# ---------------------------------------------------------------------------

def _artifact_plate(img, y0: int, y1: int) -> tuple[int, int, bytes]:
    """Reference frame with its lower band replaced by cyan/dark 2x2 blobs."""
    w, h, px = img
    out = bytearray(px)
    top = y0 + int((y1 - y0) * 0.62)
    for y in range(top, y1):
        for x in range(w):
            o = 3 * (y * w + x)
            hsh = ((x // 2) * 2654435761 + (y // 2) * 2246822519) & 0xFFFFFFFF
            hsh ^= hsh >> 15
            if hsh & 0x100:
                out[o], out[o + 1], out[o + 2] = 90, 205, 205
            else:
                out[o], out[o + 1], out[o + 2] = 12, 24, 24
    return (w, h, bytes(out))


def _invert(img):
    w, h, px = img
    return (w, h, bytes(255 - v for v in px))


def diagnose(oracle_dir: str, shift: int = SHIFT) -> dict:
    """Score a reference frame against benign and artifact transformations.

    Benign (must keep the score): 1-2 px shifts, +8 % gain.
    Artifact (must lose it): brightness inversion, a cyan speckle plate.
    Cross-frame rows give the level of two *unrelated* interiors -- the
    chance baseline of each term on this material.
    """
    frames = {}
    for name in CALIBRATION:
        path = os.path.join(oracle_dir, name + ".png")
        if os.path.exists(path):
            frames[name] = prepare_oracle(path)
    if not frames:
        return {"error": "no captures in " + oracle_dir}
    w, h = DEFAULT_SIZE
    y0, y1 = bands(h)["world"]
    names = list(frames)
    out = {"oracle_dir": oracle_dir, "shift": shift, "cases": {}}

    def add(label, ref, img):
        res = compare_images(ref, img, DEFAULT_SIZE, shift)
        m = res["metrics"]
        out["cases"][label] = {
            "score": res["score"], "shift": res["shift"],
            "edge_f1": round(m["edge_f1"]["f1"], 4),
            "colour": round(m["colour"]["score"], 4),
            "ncc": round(m["ncc"]["score"], 4),
            "ncc_signed": round(m["ncc"]["signed"], 4)}

    ref = frames[names[0]]
    add(names[0] + ": identity", ref, ref)
    add(names[0] + ": shift +1 px", ref, shift_rows(ref, 1, 0, y0, y1))
    add(names[0] + ": shift -2/+1 px", ref, shift_rows(ref, -2, 1, y0, y1))
    add(names[0] + ": gain +8%", ref, gain_luma(ref, 1.08))
    add(names[0] + ": inversion", ref, _invert(ref))
    add(names[0] + ": cyan speckle plate", ref, _artifact_plate(ref, y0, y1))
    if len(names) > 1:
        add(names[0] + " vs " + names[1] + " (cross)", ref, frames[names[1]])
        add(names[1] + " vs " + names[0] + " (cross)", frames[names[1]], ref)
    return out


def print_diagnose(rep: dict) -> None:
    if "error" in rep:
        print(rep["error"])
        return
    print(f"oracle dir {rep['oracle_dir']}  shift<={rep['shift']}")
    print(f"  {'case':34s} {'score':>6s} {'edge_f1':>7s} {'colour':>6s} "
          f"{'ncc':>5s} {'ncc_s':>6s}")
    for label, r in rep["cases"].items():
        print(f"  {label:34s} {r['score']:6.3f} {r['edge_f1']:7.3f} "
              f"{r['colour']:6.3f} {r['ncc']:5.3f} {r['ncc_signed']:+6.3f}")


# ---------------------------------------------------------------------------
# self-test (fully synthetic, no game, no capture)
# ---------------------------------------------------------------------------

def _synth(kind: str, w: int = 96, h: int = 64) -> tuple[int, int, bytes]:
    """Synthetic scene: smooth panels, straight edges, a fine grid, a floor.

    The top 60 % of the world band is structured (panels, a light bar, a fine
    grid); the bottom 40 % is a smooth floor ramp.  ``speckle`` fills exactly
    that floor with cyan/dark 2x2 blobs -- a stand-in for the ``uv="fit"``
    artifact of the real renders (docs/TEXUV.md).
    """
    px = bytearray(w * h * 3)
    floor_y = int(h * 0.70 * 0.62)          # smooth floor starts here
    for y in range(h):
        for x in range(w):
            o = 3 * (y * w + x)
            r = g = b = 18
            if 6 <= x < 44 and 4 <= y < 18:             # smooth bright panel
                r, g, b = 120, 118, 110
            elif 6 <= x < 88 and 20 <= y < 26:          # straight light bar
                r, g, b = 20, 70, 190
            elif 50 <= x < 88 and 2 <= y < 18:          # fine grid block
                v = 150 if ((x // 2) + (y // 2)) % 2 else 40
                r = g = b = v
            elif y >= floor_y:                          # smooth floor ramp
                t = (y - floor_y) / max(1, h - floor_y - 1)
                r = int(55 + 55 * t)
                g = int(50 + 45 * t)
                b = int(45 + 40 * t)
            px[o], px[o + 1], px[o + 2] = r, g, b
    if kind == "speckle":
        for y in range(floor_y, h):
            for x in range(w):
                o = 3 * (y * w + x)
                hsh = ((x // 2) * 2654435761 + (y // 2) * 2246822519) & 0xFFFFFFFF
                hsh ^= hsh >> 15
                if hsh & 0x100:
                    px[o], px[o + 1], px[o + 2] = 90, 205, 205
                else:
                    px[o], px[o + 1], px[o + 2] = 12, 24, 24
    elif kind == "invert":
        for o in range(0, len(px), 3):
            px[o] = 255 - px[o]
            px[o + 1] = 255 - px[o + 1]
            px[o + 2] = 255 - px[o + 2]
    elif kind == "gain":
        for o in range(len(px)):
            px[o] = min(255, int(px[o] * 1.08))
    return w, h, bytes(px)


def _shift(img, dx: int, dy: int):
    w, h, px = img
    return shift_rows(img, dx, dy, 0, h)


def selftest(verbose: bool = True) -> int:
    """Identity, shift, speckle, inversion, determinism -- all synthetic."""
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    base = _synth("base")
    world = {b: compare_images(base, base, (96, 64), shift=2, band=b)
             for b in ("world", "frame")}
    check("identical: score == 1", world["world"]["score"] == 1.0,
          f"{world['world']['score']:.4f}")
    m = world["world"]["metrics"]
    check("identical: every metric == 1",
          all(abs(m[k]["score" if k != "edge_f1" else "f1"] - 1.0) < 1e-9
              for k in ("edge_f1", "colour", "ncc")),
          f"e={m['edge_f1']['f1']:.3f} c={m['colour']['score']:.3f} "
          f"n={m['ncc']['score']:.3f}")

    for dx, dy in ((1, 0), (0, 1), (-2, 2), (2, -1)):
        shifted = _shift(base, dx, dy)
        r = compare_images(base, shifted, (96, 64), shift=2)
        check(f"shift {dx:+d}{dy:+d} px keeps the score (>= 0.90)",
              r["score"] >= 0.90, f"{r['score']:.4f} at {r['shift']}")

    speckle = _synth("speckle")
    r_spec = compare_images(base, speckle, (96, 64), shift=2)
    check("speckle on a smooth panel lowers the score (< 0.60)",
          r_spec["score"] < MATCH_THRESHOLD, f"{r_spec['score']:.4f}")
    check("speckle is below the shifted frame by >= 0.30",
          r_spec["score"] <= 0.90 - 0.30, f"{r_spec['score']:.4f}")

    inv = _synth("invert")
    r_inv = compare_images(base, inv, (96, 64), shift=2)
    check("brightness inversion scores ~0 (<= 0.05)", r_inv["score"] <= 0.05,
          f"{r_inv['score']:.4f}")

    gain = _synth("gain")
    r_gain = compare_images(base, gain, (96, 64), shift=2)
    check("+8% gain keeps the score (>= 0.90)", r_gain["score"] >= 0.90,
          f"{r_gain['score']:.4f}")

    a = compare_images(base, speckle, (96, 64), shift=2)
    b = compare_images(base, speckle, (96, 64), shift=2)
    check("deterministic (same input -> same JSON)",
          json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True))

    # no shift search: a 1 px translation still must not destroy the colour
    # term, and the edge tolerance must keep the edge term usable
    r0 = compare_images(base, _shift(base, 1, 1), (96, 64), shift=0)
    check("without alignment a 1 px shift still scores >= 0.65",
          r0["score"] >= 0.65, f"{r0['score']:.4f}")

    failed = [n for n, ok, _d in checks if not ok]
    if verbose:
        for name, ok, detail in checks:
            print(f"  {'ok  ' if ok else 'FAIL'} {name}"
                  + (f"  [{detail}]" if detail else ""))
        print(f"selftest: {len(checks) - len(failed)}/{len(checks)}")
    return 0 if not failed else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Noise/shift-tolerant oracle vs render frame metric.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--oracle", help="captured original-game frame (PNG)")
    ap.add_argument("--ours", help="our rendered frame (PNG)")
    ap.add_argument("--crop", help="oracle crop x,y,w,h (window borders)")
    ap.add_argument("--size", default="320x200", help="comparison grid")
    ap.add_argument("--band", choices=("world", "frame", "hud"),
                    default="world", help="band the verdict is taken on")
    ap.add_argument("--bands", action="store_true",
                    help="report frame/world/hud separately")
    ap.add_argument("--world-end", type=float, default=WORLD_END,
                    help="share of height that is world (default 0.70)")
    ap.add_argument("--shift", type=int, default=SHIFT,
                    help="search a global translation of up to N px (default 2)")
    ap.add_argument("--edge-tol", type=int, default=EDGE_TOL,
                    help="edge distance tolerance in px, 1..3 (default 2)")
    ap.add_argument("--old", action="store_true",
                    help="also print the metrics of oracle_compare.py")
    ap.add_argument("--oracle-dir", default=None,
                    help="directory with winN.png/winN.json "
                         "(for --calibrate/--diagnose)")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME",
                                                    GAME_DEFAULT))
    ap.add_argument("--diagnose", action="store_true",
                    help="sensitivity of each term to benign/artifact changes")
    ap.add_argument("--calibrate", action="store_true",
                    help="render win2/win3 in four UV modes and score them")
    ap.add_argument("--wide", action="store_true",
                    help="with --calibrate: add the per-band scores")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", dest="selftest", action="store_true")
    opts = ap.parse_args(argv)

    if opts.selftest:
        return selftest()
    if opts.diagnose:
        rep = diagnose(find_oracle_dir(opts.oracle_dir), opts.shift)
        if opts.json:
            print(json.dumps(rep, ensure_ascii=False, indent=2))
        else:
            print_diagnose(rep)
        return 0
    if opts.calibrate:
        rep = calibrate(find_oracle_dir(opts.oracle_dir), opts.game,
                        opts.shift, opts.edge_tol, opts.wide)
        if opts.json:
            print(json.dumps(rep, ensure_ascii=False, indent=2))
        else:
            print_calibration(rep)
        return 0
    if not opts.oracle or not opts.ours:
        ap.error("--oracle and --ours are required (or --self-test/--calibrate)")

    tw, th = (int(v) for v in opts.size.lower().split("x"))
    crop_box = tuple(int(v) for v in opts.crop.split(",")) if opts.crop else None
    result = compare_files(
        opts.oracle, opts.ours, (tw, th), opts.shift, opts.edge_tol,
        opts.band, opts.bands, None, opts.world_end, opts.old, crop_box)
    if opts.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result.get("match", True) else 1
    if opts.bands:
        print(f"oracle {opts.oracle}\nours   {opts.ours}")
        for name, res in result["bands"].items():
            m = res["metrics"]
            print(f"{name:5s} score={res['score']:.3f} "
                  f"{'match' if res['match'] else 'differs'}  "
                  f"eF1={m['edge_f1']['f1']:.3f} "
                  f"colour={m['colour']['score']:.3f} "
                  f"ncc={m['ncc']['score']:.3f} shift={res['shift']}")
            if opts.old:
                old = result["old"][name]
                print(f"      old: corr={old['corr']:+.3f} mad={old['mad']:.3f} "
                      f"edges={old['edges']:.3f} hist={old['hist']:.3f}")
    else:
        m = result["metrics"]
        print(f"oracle {opts.oracle}\nours   {opts.ours}")
        print(f"band   {result['band']} rows {result['band_rows']}"
              f"  shift {result['shift']}  threshold {result['threshold']:.2f}")
        print(f"score  {result['score']:.3f}  "
              f"{'match' if result['match'] else 'differs'}")
        print(f"edge_f1 {m['edge_f1']['f1']:.3f} "
              f"(P={m['edge_f1']['precision']:.3f} R={m['edge_f1']['recall']:.3f} "
              f"tol={m['edge_f1']['tol']})")
        print(f"colour  {m['colour']['score']:.3f} "
              f"(chi2={m['colour']['chi2']:.3f}, {m['colour']['blocks']} blocks, "
              f"gain={m['colour']['gain']})")
        print(f"ncc     {m['ncc']['score']:.3f} "
              f"({m['ncc']['blocks']} blocks, {m['ncc']['noise_blocks']} noise)")
        if opts.old:
            old = result["old"][result["band"]]
            print(f"old     corr={old['corr']:+.3f} mad={old['mad']:.3f} "
                  f"edges={old['edges']:.3f} hist={old['hist']:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
