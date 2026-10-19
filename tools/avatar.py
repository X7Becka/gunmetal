#!/usr/bin/env python3
"""Render a social-media avatar from a Gunmetal `.sob` model.

Two families of looks are provided:

``hero`` / ``emblem``
    Artistic: supersampling, three-point lighting with a warm rim, a metal
    plate background and a segmented reticle.  Textures are expanded with the
    **PICS** palette (the FLAG BMPs that `gm.palette.installed()` prefers have
    240..255 filled with magenta; the level/model art uses the other palette
    family, e.g. `PICS/mine-1c.bmp`).

``ingame``
    Faithful to the engine.  It reproduces what the 1998 renderer puts on
    screen rather than an art pass:

    * the **GAMEPAL** palette from `FRONTEND/BMP.IDX` (the level/model art),
    * the engine's planar auto-UV: **one texel per world unit**, the texture
      plane anchored at each face's first vertex (the `.sob` loader passes a
      zero reference, so `texel = uv/256 + dot(axis, v - v0)`),
    * the engine's flat lighting `AMBIENT + (1-AMBIENT)*|dot(n, L)|` plus the
      `.sob` face brightness `flags/127 * BRIGHTNESS_GAIN`,
    * bilinear sampling, which stands in for the Glide/3dfx bilinear filter.

    Framing is automatic: the tank is cropped to its alpha bounding box and
    centred, on a dark in-game backdrop.  Variants of angle and background can
    be emitted as a contact sheet.

Examples::

    python3 tools/avatar.py --out out/avatar
    python3 tools/avatar.py --model Spartan --size 512 --out out/avatar
    python3 tools/avatar.py --styles ingame --model Tick --size 1024 \\
        --az 32 --el 16 --out out/avatar_ingame
    python3 tools/avatar.py --styles ingame --model Tick --sheet out/tick.png
"""

from __future__ import annotations

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gm import bigfile, bmp, palette as palmod, png, render, sob  # noqa: E402
from gm.render import Camera, _clip_near, _dot, _face_normal, _norm, _sub  # noqa: E402

GAME_DEFAULT = "/mnt/d/games/Gunmetal 3dfx"
PALETTE_CANDIDATES = ("PICS/mine-1c.bmp", "PICS/mine-1d.bmp", "FLAG/naUSA.BMP")


# ---------------------------------------------------------------------------
# geometry / textures
# ---------------------------------------------------------------------------


def scale_uv(model: sob.Sob, factor: float) -> sob.Sob:
    """Map world units to texture fractions.

    ``factor = 1/256`` lands one texel per world unit for a 256² atlas.  That
    keeps the atlas coherent across neighbouring faces instead of stretching
    the whole texture over every face (the viewer's ``--uv fit`` default), so
    the markings land in plausible places.
    """
    faces = [
        sob.SobFace(
            f.textures, f.flags, f.indices, f.origin,
            tuple(c * factor for c in f.u), tuple(c * factor for c in f.v),
            f.normal, f.spare,
        )
        for f in model.faces
    ]
    return sob.Sob(model.vertices, faces, model.name)


class Mesh:
    def __init__(self, model: sob.Sob):
        self.vertices = model.vertices
        self.faces = model.faces

    def uv(self, face, vertex):
        d = (vertex[0] - face.origin[0], vertex[1] - face.origin[1],
             vertex[2] - face.origin[2])
        return (sum(a * b for a, b in zip(d, face.u)) / 65536.0,
                sum(a * b for a, b in zip(d, face.v)) / 65536.0)


class GameMesh:
    """Faces textured the way the engine does.

    The engine runs its auto-UV generator on every face (`.sob` clears the
    "explicit UV" bit), and the loader passes a *zero* reference point, so the
    texture plane is anchored at the face's first vertex and the per-unit step
    is the face axis::

        texel = uv/256 + dot(axis, vertex - v0)      (one texel per world unit)

    ``face.origin`` is read as ``(marker, uv_u, uv_v)`` and ``face.u`` /
    ``face.v`` are unit axes scaled by 65536, hence the divisions below.  The
    returned coordinates are in texels; ``render_rgba(..., texel_uv=True)``
    normalises them by the texture size before sampling.
    """

    def __init__(self, model: sob.Sob):
        self.vertices = model.vertices
        self.faces = model.faces

    def uv(self, face, vertex):
        a = self.vertices[face.indices[0]]
        ux, uy, uz = (c / 65536.0 for c in face.u)
        vx, vy, vz = (c / 65536.0 for c in face.v)
        dx, dy, dz = vertex[0] - a[0], vertex[1] - a[1], vertex[2] - a[2]
        return (face.origin[1] / 256.0 + ux * dx + uy * dy + uz * dz,
                face.origin[2] / 256.0 + vx * dx + vy * dy + vz * dz)


def find_palette(game: str) -> bytes:
    for rel in PALETTE_CANDIDATES:
        path = os.path.join(game, rel)
        if os.path.exists(path):
            return palmod.load_from_bmp(path)
    return palmod.installed(game)


def game_palette(game: str) -> bytes:
    """The in-game palette: the ``GAMEPAL`` entry of ``FRONTEND/BMP.IDX``.

    This is the 256-colour table the engine itself uses for levels and models
    (a tiny 8x8 BMP), so it is the honest choice for an "as in the game" look.
    Falls back to the PICS art palette when the archive is unavailable.
    """
    idx = os.path.join(game, "FRONTEND", "BMP.IDX")
    if os.path.exists(idx):
        try:
            bf = bigfile.BigFile(idx)
            for entry in bf.entries:
                if entry.name.upper() == "GAMEPAL":
                    with open(bf.dat_path, "rb") as fh:
                        data = fh.read()[entry.offset:entry.offset + entry.size]
                    return bmp.parse(data).palette
        except (OSError, ValueError):
            pass
    return find_palette(game)


def bilinear(tex, u: float, v: float):
    w, h, px = tex
    u %= 1.0
    v %= 1.0
    x = u * w - 0.5
    y = v * h - 0.5
    x0 = math.floor(x)
    y0 = math.floor(y)
    fx, fy = x - x0, y - y0
    x0 %= w
    y0 %= h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    o00, o10 = (y0 * w + x0) * 3, (y0 * w + x1) * 3
    o01, o11 = (y1 * w + x0) * 3, (y1 * w + x1) * 3
    out = []
    for k in range(3):
        top = px[o00 + k] + (px[o10 + k] - px[o00 + k]) * fx
        bot = px[o01 + k] + (px[o11 + k] - px[o01 + k]) * fx
        out.append(top + (bot - top) * fy)
    return out


# ---------------------------------------------------------------------------
# lighting
# ---------------------------------------------------------------------------

KEY = _norm((-0.55, 0.35, 0.86))
KEY_COL = (1.14, 1.03, 0.90)
FILL = _norm((0.65, -0.25, 0.40))
FILL_COL = (0.30, 0.45, 0.66)
RIM = _norm((0.70, -0.50, 0.25))
RIM_COL = (1.12, 0.58, 0.20)
BASE = 0.40


def shade(n, view):
    """Return per-channel multipliers and a scalar specular term."""
    n = _norm(n)
    to_eye = _norm((-view[0], -view[1], -view[2]))
    if _dot(n, to_eye) < 0:
        n = (-n[0], -n[1], -n[2])
    kd = max(0.0, _dot(n, KEY))
    fd = max(0.0, _dot(n, FILL)) * 0.30
    rd = (max(0.0, _dot(n, RIM))
          * pow(max(0.0, 1.0 - abs(_dot(n, to_eye))), 2.0) * 0.55)
    amb = BASE + 0.10 * max(0.0, n[2])
    bounce = max(0.0, -n[2]) * 0.18
    h = _norm((KEY[0] + to_eye[0], KEY[1] + to_eye[1], KEY[2] + to_eye[2]))
    spec = pow(max(0.0, _dot(n, h)), 30.0) * 0.26
    return (amb + KEY_COL[0] * kd + FILL_COL[0] * fd + RIM_COL[0] * rd + bounce * 0.70,
            amb + KEY_COL[1] * kd + FILL_COL[1] * fd + RIM_COL[1] * rd + bounce * 0.45,
            amb + KEY_COL[2] * kd + FILL_COL[2] * fd + RIM_COL[2] * rd + bounce * 0.28,
            spec)


# --- the engine's own lighting (see gm/render.py's AMBIENT/LIGHT note) -----

LIGHT = _norm((0.42, -0.5, 0.75))
AMBIENT = 0.28
BRIGHTNESS_GAIN = 0.5


def engine_shade(face) -> float:
    """Scalar brightness the game multiplies the texel by.

    ``AMBIENT + (1-AMBIENT)*|dot(n, L)|`` is the renderer's flat lambert
    (absolute value: faces are two-sided), and ``flags/127`` is the per-face
    brightness stored in the `.sob` (``poly+0x70``), scaled by the same gain
    the toolkit uses.
    """
    n = _norm(_face_normal(face))
    lam = abs(_dot(n, LIGHT))
    return (AMBIENT + (1.0 - AMBIENT) * lam
            + (face.flags / 127.0) * BRIGHTNESS_GAIN)


# ---------------------------------------------------------------------------
# rasteriser (RGBA, supersampled, bilinear)
# ---------------------------------------------------------------------------


def render_rgba(mesh, size, az, el, zoom, fov, pool, shade_fn=None, texel_uv=False):
    center, radius = render._bounds([mesh])
    cam = Camera(center, radius, size, size, az, el, zoom, fov)
    eye = cam.eye
    buf = bytearray(size * size * 3)
    alpha = bytearray(size * size)
    zbuf = [-1.0] * (size * size)

    for face in mesh.faces:
        idx = face.indices
        if len(idx) < 3:
            continue
        tex = pool.get(face.texture)
        view = [cam.view(mesh.vertices[i]) for i in idx]
        uvs = [mesh.uv(face, mesh.vertices[i]) for i in idx]
        if texel_uv:
            tw, th = (tex[0], tex[1]) if tex else (1, 1)
            uvs = [(u / tw, v / th) for u, v in uvs]
        if any(v[2] < cam.near for v in view):
            view, groups = _clip_near(view, [uvs], cam.near)
            if len(view) < 3:
                continue
            uvs = groups[0]
        pts = [cam.project_view(v) for v in view]
        if any(p is None for p in pts):
            continue
        if shade_fn is None:
            sh = shade(_face_normal(face), _norm(_sub(mesh.vertices[idx[0]], eye)))
        else:
            s = shade_fn(face)
            sh = (s, s, s, 0.0)
        for k in range(1, len(pts) - 1):
            triangle(buf, alpha, zbuf, size,
                     (pts[0], pts[k], pts[k + 1]),
                     (uvs[0], uvs[k], uvs[k + 1]), tex, sh)
    return bytes(buf), bytes(alpha)


def triangle(buf, alpha, zbuf, size, tri, tuv, tex, sh):
    (x0, y0, w0, _z0), (x1, y1, w1, _z1), (x2, y2, w2, _z2) = tri
    minx = max(0, int(min(x0, x1, x2)))
    maxx = min(size - 1, int(max(x0, x1, x2)) + 1)
    miny = max(0, int(min(y0, y1, y2)))
    maxy = min(size - 1, int(max(y0, y1, y2)) + 1)
    if maxx < minx or maxy < miny:
        return
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(area) < 1e-9:
        return
    inv = 1.0 / area
    rk, gk, bk, spec = sh
    for y in range(miny, maxy + 1):
        py = y + 0.5
        for x in range(minx, maxx + 1):
            px = x + 0.5
            l0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv
            if l0 < 0:
                continue
            l1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv
            if l1 < 0:
                continue
            l2 = 1.0 - l0 - l1
            if l2 < 0:
                continue
            i = y * size + x
            iz = l0 * w0 + l1 * w1 + l2 * w2
            if iz <= zbuf[i]:
                continue
            zbuf[i] = iz
            alpha[i] = 255
            if tex is None:
                r, g, b = (92, 92, 102)
            else:
                uu = (l0 * tuv[0][0] * w0 + l1 * tuv[1][0] * w1
                      + l2 * tuv[2][0] * w2) / iz
                vv = (l0 * tuv[0][1] * w0 + l1 * tuv[1][1] * w1
                      + l2 * tuv[2][1] * w2) / iz
                r, g, b = bilinear(tex, uu, vv)
            lum = (r + g + b) / 765.0
            sp = spec * (0.35 + 0.65 * lum)
            r = r * rk + 255.0 * sp
            g = g * gk + 255.0 * sp
            b = b * bk + 255.0 * sp
            o = i * 3
            buf[o] = _byte(r)
            buf[o + 1] = _byte(g)
            buf[o + 2] = _byte(b)


def _byte(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def downsample(buf, alpha, size, factor):
    ow = size // factor
    n = factor * factor
    out = bytearray(ow * ow * 4)
    for oy in range(ow):
        for ox in range(ow):
            rs = gs = bs = asum = 0
            for dy in range(factor):
                row = (oy * factor + dy) * size
                for dx in range(factor):
                    i = row + ox * factor + dx
                    o = i * 3
                    a = alpha[i]
                    rs += buf[o] * a
                    gs += buf[o + 1] * a
                    bs += buf[o + 2] * a
                    asum += a
            k = oy * ow + ox
            if asum:
                out[k * 4] = rs // asum
                out[k * 4 + 1] = gs // asum
                out[k * 4 + 2] = bs // asum
                out[k * 4 + 3] = asum // n
    return bytes(out)


# ---------------------------------------------------------------------------
# background / compositing
# ---------------------------------------------------------------------------


def _smooth(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def _hash(x, y):
    n = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0


def background(size):
    """Dark metal plate, amber backlight, four-arc reticle, grain, vignette."""
    buf = bytearray(size * size * 3)
    c = size / 2.0
    for y in range(size):
        for x in range(size):
            dx = (x - c) / size
            dy = (y - c) / size
            r = math.sqrt(dx * dx + dy * dy)
            ang = math.atan2(dy, dx)
            plate = 1.0 - _smooth(0.475, 0.488, r)
            inner = 1.0 - _smooth(0.0, 0.49, r)
            streak = (_hash(0, y // 2) * 2.0 - 1.0) * 2.6 * inner
            v = inner * (0.55 + 0.45 * (1.0 - r / 0.49))
            rr = 5.0 + (6.0 + 6.0 * v + streak) * plate
            gg = 6.0 + (8.0 + 9.0 * v + streak) * plate
            bb = 9.0 + (11.0 + 14.0 * v + streak * 1.15) * plate

            gx = (x - c) / size
            gy = (y - c * 0.98) / size
            gr = math.sqrt(gx * gx * 1.50 + gy * gy * 2.30)
            glow = max(0.0, 1.0 - _smooth(0.02, 0.34, gr)) ** 1.65
            rr += 108.0 * glow
            gg += 43.0 * glow
            bb += 7.0 * glow

            for centre in (45, 135, 225, 315):
                a = math.radians(centre)
                da = (ang - a + math.pi) % (2.0 * math.pi) - math.pi
                if abs(da) < math.radians(26.0):
                    aa = max(0.0, 1.0 - abs(r - 0.417) * size * 2.0)
                    rr += aa * 50.0
                    gg += aa * 53.0
                    bb += aa * 58.0
            aa = max(0.0, 1.0 - abs(r - 0.445) * size * 2.6)
            rr += aa * 23.0
            gg += aa * 25.0
            bb += aa * 28.0

            vig = 1.0 - _smooth(0.50, 1.03, r)
            k = 0.40 + 0.60 * vig
            rr *= k
            gg *= k
            bb *= k
            grain = (_hash(x, y) - 0.5) * 4.0
            o = (y * size + x) * 3
            buf[o] = _byte(rr + grain)
            buf[o + 1] = _byte(gg + grain)
            buf[o + 2] = _byte(bb + grain)
    return buf


def box_blur(src, w, h, radius, passes=3):
    tmp = [0.0] * (w * h)
    out = list(src)
    span = 2 * radius + 1
    for _ in range(passes):
        for y in range(h):
            row = y * w
            acc = 0.0
            for x in range(-radius, radius + 1):
                if 0 <= x < w:
                    acc += out[row + x]
            for x in range(w):
                tmp[row + x] = acc / span
                if x - radius >= 0:
                    acc -= out[row + x - radius]
                if x + radius + 1 < w:
                    acc += out[row + x + radius + 1]
        for x in range(w):
            acc = 0.0
            for y in range(-radius, radius + 1):
                if 0 <= y < h:
                    acc += tmp[y * w + x]
            for y in range(h):
                out[y * w + x] = acc / span
                if y - radius >= 0:
                    acc -= tmp[(y - radius) * w + x]
                if y + radius + 1 < h:
                    acc += tmp[(y + radius + 1) * w + x]
    return out


def silhouette_glow(alpha, size, radius):
    f = 4
    w = size // f
    small = [0.0] * (w * w)
    for y in range(w):
        for x in range(w):
            s = 0
            for dy in range(f):
                row = (y * f + dy) * size
                for dx in range(f):
                    s += alpha[row + x * f + dx]
            small[y * w + x] = s / (f * f * 255.0)
    blurred = box_blur(small, w, w, max(1, radius // f), 3)
    out = [0.0] * (size * size)
    for y in range(size):
        sy = y / f - 0.5
        y0 = max(0, min(w - 1, int(math.floor(sy))))
        y1 = min(w - 1, y0 + 1)
        fy = sy - y0
        for x in range(size):
            sx = x / f - 0.5
            x0 = max(0, min(w - 1, int(math.floor(sx))))
            x1 = min(w - 1, x0 + 1)
            fx = sx - x0
            top = blurred[y0 * w + x0] + (blurred[y0 * w + x1] - blurred[y0 * w + x0]) * fx
            bot = blurred[y1 * w + x0] + (blurred[y1 * w + x1] - blurred[y1 * w + x0]) * fx
            out[y * size + x] = top + (bot - top) * fy
    return out


def composite(bg, tank, alpha, size, glow):
    out = bytearray(bg)
    for i in range(size * size):
        a = glow[i]
        if a > 0.002:
            o = i * 3
            out[o] = _byte(out[o] + a * 92.0)
            out[o + 1] = _byte(out[o + 1] + a * 38.0)
            out[o + 2] = _byte(out[o + 2] + a * 8.0)
    for i in range(size * size):
        a = alpha[i] / 255.0
        if a <= 0.0:
            continue
        o = i * 3
        p = i * 4
        out[o] = _byte(tank[p] * a + out[o] * (1.0 - a))
        out[o + 1] = _byte(tank[p + 1] * a + out[o + 1] * (1.0 - a))
        out[o + 2] = _byte(tank[p + 2] * a + out[o + 2] * (1.0 - a))
    return out


def border(buf, size):
    c = size / 2.0
    for y in range(size):
        for x in range(size):
            dx = x + 0.5 - c
            dy = y + 0.5 - c
            r = math.sqrt(dx * dx + dy * dy) / size
            a = max(0.0, 1.0 - abs(r - 0.486) * size * 1.2)
            if a <= 0:
                continue
            t = 0.5 + 0.5 * math.sin(math.atan2(dy, dx) + 0.6)
            col = (150 + (122 - 150) * t, 172 + (96 - 172) * t, 192 + (58 - 192) * t)
            o = (y * size + x) * 3
            for k in range(3):
                buf[o + k] = _byte(buf[o + k] * (1.0 - a * 0.85) + col[k] * a * 0.85)


def circle_rgba(rgb, size):
    out = bytearray(size * size * 4)
    r = size / 2.0 - 1.0
    for y in range(size):
        for x in range(size):
            dx = x + 0.5 - size / 2.0
            dy = y + 0.5 - size / 2.0
            a = max(0.0, min(1.0, r - math.sqrt(dx * dx + dy * dy) + 0.5))
            o = (y * size + x) * 4
            p = (y * size + x) * 3
            out[o:o + 3] = rgb[p:p + 3]
            out[o + 3] = int(a * 255.0)
    return bytes(out)


def write(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)
    print(path)


def _render_layer(mesh, pool, size, az, el, zoom, fov):
    factor = 3
    buf, alpha = render_rgba(mesh, size * factor, az, el, zoom, fov, pool)
    tank = downsample(buf, alpha, size * factor, factor)
    return tank, tank[3::4]


def grade_hero(tank, size, alpha):
    """Lift the 1998 textures out of the mud and add a thin rim."""
    out = bytearray(tank)
    for i in range(size * size):
        p = i * 4
        if not tank[p + 3]:
            continue
        r, g, b = tank[p] / 255.0, tank[p + 1] / 255.0, tank[p + 2] / 255.0
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        r = lum + (r - lum) * 1.18
        g = lum + (g - lum) * 1.18
        b = lum + (b - lum) * 1.18
        r = max(0.0, min(1.0, r)) ** 0.82
        g = max(0.0, min(1.0, g)) ** 0.82
        b = max(0.0, min(1.0, b)) ** 0.82
        if lum < 0.5:
            b = min(1.0, b + 0.035)
            r = max(0.0, r - 0.01)
        else:
            r = min(1.0, r + 0.02)
        out[p] = int(r * 255.0)
        out[p + 1] = int(g * 255.0)
        out[p + 2] = int(b * 255.0)

    # one-pixel rim, warm on the left and cool on the right
    edge = bytearray(size * size)
    for y in range(1, size - 1):
        for x in range(1, size - 1):
            i = y * size + x
            if alpha[i] < 40:
                continue
            if (alpha[i - 1] < 20 or alpha[i + 1] < 20
                    or alpha[i - size] < 20 or alpha[i + size] < 20):
                edge[i] = 255
    for i in range(size * size):
        if not edge[i]:
            continue
        p = i * 4
        t = (i % size) / max(1, size - 1)
        col = (int(255 * (1 - t) + 120 * t),
               int(170 * (1 - t) + 210 * t),
               int(90 * (1 - t) + 255 * t))
        for k in range(3):
            out[p + k] = _byte(out[p + k] * 0.65 + col[k] * 0.35)
    return out


def run_style_hero(mesh, pool, size, az, el, zoom, fov):
    tank, alpha = _render_layer(mesh, pool, size, az, el, zoom, fov)
    tank = grade_hero(tank, size, alpha)
    bg = background(size)
    glow = silhouette_glow(alpha, size, max(4, size // 30))
    out = composite(bg, tank, alpha, size, glow)
    border(out, size)
    return out


def run_style_emblem(mesh, pool, size, az, el, zoom, fov):
    """Steel/ember duotone with the original cyan/red accents kept."""
    tank, alpha = _render_layer(mesh, pool, size, az, el, zoom, fov)
    duo = bytearray(size * size * 4)
    for i in range(size * size):
        p = i * 4
        if not tank[p + 3]:
            continue
        r, g, b = tank[p], tank[p + 1], tank[p + 2]
        lum = (r * 0.299 + g * 0.587 + b * 0.114) / 255.0
        lum = min(1.0, max(0.0, (lum - 0.10) * 1.55))
        if lum < 0.62:
            t = lum / 0.62
            rr = 8 + (118 - 8) * t
            gg = 13 + (140 - 13) * t
            bb = 20 + (164 - 20) * t
        else:
            t = (lum - 0.62) / 0.38
            rr = 118 + (255 - 118) * t
            gg = 140 + (226 - 140) * t
            bb = 164 + (182 - 164) * t
        if b > 110 and b > r + 55 and g > r + 30:
            rr, gg, bb = 70, 230, 255
        elif r > 110 and r > g * 1.45 and r > b * 1.35:
            rr, gg, bb = 255, 86, 40
        elif r > 150 and g > 100 and b < 90:
            rr, gg, bb = 255, 178, 64
        duo[p] = int(rr)
        duo[p + 1] = int(gg)
        duo[p + 2] = int(bb)
        duo[p + 3] = tank[p + 3]
    bg = background(size)
    glow = silhouette_glow(alpha, size, max(3, size // 42))
    out = composite(bg, duo, alpha, size, glow)
    border(out, size)
    return out


# ---------------------------------------------------------------------------
# in-game style: engine UV + engine lighting, auto-framed
# ---------------------------------------------------------------------------


def alpha_bbox(rgba, size):
    """Bounding box of the non-transparent pixels (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = size, size, -1, -1
    for y in range(size):
        row = y * size
        for x in range(size):
            if rgba[(row + x) * 4 + 3]:
                x0 = min(x0, x)
                x1 = max(x1, x)
                y0 = min(y0, y)
                y1 = max(y1, y)
    return x0, y0, x1, y1


def _sample_rgba(rgba, size, sx, sy):
    if sx < 0 or sy < 0 or sx > size - 1 or sy > size - 1:
        return (0.0, 0.0, 0.0, 0.0)
    x0, y0 = int(sx), int(sy)
    x1 = min(size - 1, x0 + 1)
    y1 = min(size - 1, y0 + 1)
    fx, fy = sx - x0, sy - y0
    out = [0.0, 0.0, 0.0, 0.0]
    for xx, yy, w in ((x0, y0, (1 - fx) * (1 - fy)), (x1, y0, fx * (1 - fy)),
                      (x0, y1, (1 - fx) * fy), (x1, y1, fx * fy)):
        o = (yy * size + xx) * 4
        for k in range(4):
            out[k] += rgba[o + k] * w
    return out


def framed_tank(size, rgba, res, bbox, fill=0.88):
    """Scale/centre the tank (alpha box) into a size x size RGBA canvas."""
    x0, y0, x1, y1 = bbox
    bw, bh = x1 - x0 + 1, y1 - y0 + 1
    scale = (size * fill) / max(bw, bh)
    ox = (size - bw * scale) / 2.0
    oy = (size - bh * scale) / 2.0
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            px = _sample_rgba(rgba, res, x0 + (x - ox) / scale, y0 + (y - oy) / scale)
            o = (y * size + x) * 4
            out[o] = _byte(px[0])
            out[o + 1] = _byte(px[1])
            out[o + 2] = _byte(px[2])
            out[o + 3] = _byte(px[3])
    return out


BG_PRESETS = {
    "dark": ((14, 17, 22), (30, 36, 46)),
    "steel": ((18, 19, 21), (54, 57, 61)),
    "green": ((10, 16, 12), (30, 46, 32)),
    "black": ((2, 2, 3), (12, 12, 14)),
}


def bg_square(x, y, size, preset="dark"):
    """Flat in-game backdrop: a soft radial gradient, one of BG_PRESETS."""
    lo, hi = BG_PRESETS[preset]
    c = (size - 1) / 2.0
    dx = (x - c) / size
    dy = (y - c) / size
    v = 1.0 - min(1.0, math.sqrt(dx * dx + dy * dy) / 0.78)
    v = v * v * (3.0 - 2.0 * v)
    return tuple(lo[k] + (hi[k] - lo[k]) * v for k in range(3))


def render_ingame_layer(mesh, pool, res, az, el, zoom, fov, ss=2):
    """Render the tank engine-style and return an ``res``-sized RGBA layer."""
    buf, alpha = render_rgba(mesh, res * ss, az, el, zoom, fov, pool,
                             shade_fn=engine_shade, texel_uv=True)
    return downsample(buf, alpha, res * ss, ss)


def run_style_ingame(mesh, pool, size, az, el, zoom, fov, bg="dark", fill=0.88,
                     res=None):
    """Return ``(rgb, rgba)``: the framed tank on ``bg`` plus the raw layer."""
    res = res or max(size, 1000)
    rgba = render_ingame_layer(mesh, pool, res, az, el, zoom, fov, ss=2)
    tank = framed_tank(size, rgba, res, alpha_bbox(rgba, res), fill)
    rgb = bytearray(size * size * 3)
    for y in range(size):
        for x in range(size):
            i = y * size + x
            a = tank[i * 4 + 3] / 255.0
            b = bg_square(x, y, size, bg)
            o = i * 3
            for k in range(3):
                rgb[o + k] = _byte(tank[i * 4 + k] * a + b[k] * (1.0 - a))
    return bytes(rgb), tank


# (azimuth, elevation, background) tiles for the contact sheet; printed in
# reading order so the sheet needs no text overlay.
SHEET_VIEWS = (
    (32, 16, "dark"), (0, 16, "dark"), (60, 16, "dark"),
    (32, 8, "dark"), (32, 28, "dark"), (32, 16, "steel"),
    (-40, 18, "dark"), (90, 16, "dark"), (32, 16, "green"),
    (32, 55, "dark"), (32, 16, "black"), (140, 20, "dark"),
)


def write_sheet(mesh, pool, path, tile=384, zoom=1.0, fov=30.0, cols=3):
    rows = (len(SHEET_VIEWS) + cols - 1) // cols
    W, H = cols * tile, rows * tile
    sheet = bytearray(W * H * 3)
    for k, (az, el, bg) in enumerate(SHEET_VIEWS):
        rgb, _ = run_style_ingame(mesh, pool, tile, az, el, zoom, fov,
                                  bg=bg, res=tile * 2)
        cx, cy = (k % cols) * tile, (k // cols) * tile
        for y in range(tile):
            ro = ((cy + y) * W + cx) * 3
            po = y * tile * 3
            sheet[ro:ro + tile * 3] = rgb[po:po + tile * 3]
    write(path, png.rgb(W, H, bytes(sheet)))
    for k, (az, el, bg) in enumerate(SHEET_VIEWS):
        print(f"  [{k % cols},{k // cols}] az={az:g} el={el:g} bg={bg}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=GAME_DEFAULT, help="Gunmetal installation")
    ap.add_argument("--model", default="Devsttr",
                    help="model name under SOBS/ (default: Devsttr)")
    ap.add_argument("--out", default="out/avatar", help="output directory")
    ap.add_argument("--size", type=int, default=1024, help="square size (default 1024)")
    ap.add_argument("--az", type=float, default=35.0, help="azimuth (default 35)")
    ap.add_argument("--el", type=float, default=20.0, help="elevation (default 20)")
    ap.add_argument("--zoom", type=float, default=1.05, help="zoom (default 1.05)")
    ap.add_argument("--fov", type=float, default=30.0, help="field of view (default 30)")
    ap.add_argument("--uv-scale", type=float, default=1.0 / 256.0,
                    help="world units -> texture fraction, hero/emblem (default 1/256)")
    ap.add_argument("--styles", default="hero,emblem",
                    help="comma separated: hero,emblem,ingame (default hero,emblem)")
    ap.add_argument("--palette", choices=("auto", "game", "pics"), default="auto",
                    help="auto (game for ingame, else pics), game or pics")
    ap.add_argument("--bg", default="dark",
                    help="ingame backdrop: " + ", ".join(sorted(BG_PRESETS)))
    ap.add_argument("--fill", type=float, default=0.88,
                    help="ingame framing fill fraction (default 0.88)")
    ap.add_argument("--sheet", default=None,
                    help="render angle/background variants to this PNG and exit")
    ap.add_argument("--sheet-tile", type=int, default=384,
                    help="contact-sheet tile size (default 384)")
    args = ap.parse_args(argv)

    model_path = os.path.join(args.game, "SOBS", args.model + ".sob")
    if not os.path.exists(model_path):
        ap.error(f"model not found: {model_path}")
    styles = [s.strip() for s in args.styles.split(",") if s.strip()]

    game_pal = args.palette == "game" or (args.palette == "auto" and "ingame" in styles)
    pal = game_palette(args.game) if game_pal else find_palette(args.game)
    pool = render.TexturePool(args.game, pal=pal)

    model = sob.parse(model_path)
    mesh = Mesh(scale_uv(model, args.uv_scale))
    game_mesh = GameMesh(model)

    if args.sheet:
        write_sheet(game_mesh, pool, args.sheet, tile=args.sheet_tile,
                    zoom=args.zoom, fov=args.fov)
        return

    for style in styles:
        if style == "hero":
            out = run_style_hero(mesh, pool, args.size, args.az, args.el,
                                 args.zoom, args.fov)
            name = "avatar_hero"
        elif style == "emblem":
            out = run_style_emblem(mesh, pool, args.size, args.az, args.el,
                                   args.zoom, args.fov)
            name = "avatar_emblem"
        elif style == "ingame":
            rgb, tank = run_style_ingame(mesh=game_mesh, pool=pool, size=args.size,
                                         az=args.az, el=args.el, zoom=args.zoom,
                                         fov=args.fov, bg=args.bg, fill=args.fill)
            write(os.path.join(args.out, f"avatar_ingame_{args.size}.png"),
                  png.rgb(args.size, args.size, rgb))
            write(os.path.join(args.out, f"avatar_ingame_{args.size}_circle.png"),
                  png.rgba(args.size, args.size, circle_rgba(rgb, args.size)))
            write(os.path.join(args.out, f"tank_ingame_{args.size}.png"),
                  png.rgba(args.size, args.size, bytes(tank)))
            continue
        else:
            ap.error(f"unknown style: {style}")
        rgb = bytes(out)
        write(os.path.join(args.out, f"{name}_{args.size}.png"),
              png.rgb(args.size, args.size, rgb))
        circle = circle_rgba(rgb, args.size)
        write(os.path.join(args.out, f"{name}_{args.size}_circle.png"),
              png.rgba(args.size, args.size, circle))
        if style == "hero":
            write(os.path.join(args.out, f"avatar_{args.size}.png"),
                  png.rgb(args.size, args.size, rgb))
            write(os.path.join(args.out, f"avatar_circle_{args.size}.png"),
                  png.rgba(args.size, args.size, circle))


if __name__ == "__main__":
    main()
