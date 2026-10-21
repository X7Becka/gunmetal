#!/usr/bin/env python3
"""Независимый оракул для голденов ``engine/tests/test_collide.cpp``.

Скрипт читает ``.sbw``, применяет **доказанный** трансформ объекта (96-байтовый
заголовок = 12 ``f64``: ``origin[3] + basis[3][3]``, ``world = origin + basis *
local``; базис в поставке единичный), триангулирует полигоны веером и считает
те же величины, что проверяет C++-тест: AABB мира и сектора, разбиение
поверхностей по порогу ``|n.z| = 0.8``, лучи (Мёллер-Трумбер, двусторонний),
``floor_at``, счётчики broad-phase и луч вниз для golden'а сферы.

Зачем отдельный скрипт: у него нет ни одной общей строки с ``engine/src``,
поэтому общая ошибка в C++ (загрузка трансформа, триангуляция, epsilon'ы) не
может «согласовать» оба результата. Числа печатаются в виде, который можно
вставить в тест.

    python3 tools/re/collide_goldens.py                 # level0, всё
    python3 tools/re/collide_goldens.py level3 --json   # другой уровень

Только чтение установки игры.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbw  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
EYE = (24.0, -208.0, 58.0)      # level0 spawn slot 0 + eye 10
SPHERE_Z = 300.0
SPHERE_R = 10.0
INF = 1e30
RAY_EPS = 1e-9
BAR_EPS = 1e-9
MIN_NZ = 0.8


def _v(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _len(a):
    return math.sqrt(_dot(a, a))


def _norm(a):
    n = _len(a)
    return _mul(a, 1.0 / n) if n > 0.0 else (0.0, 0.0, 0.0)


class Triangle:
    __slots__ = ("a", "b", "c", "normal", "lo", "hi", "sector", "object",
                 "texture", "material", "lightmap", "poly")

    def surface(self):
        if self.normal[2] >= MIN_NZ:
            return "floor"
        if self.normal[2] <= -MIN_NZ:
            return "ceiling"
        return "wall"


class World:
    """Треугольники уровня в мировом кадре (origin + basis * local)."""

    def __init__(self, level: "sbw.Sbw"):
        self.tris: list[Triangle] = []
        self.sector_ranges: list[tuple[int, int]] = []
        for si, sector in enumerate(level.sectors):
            begin = len(self.tris)
            for oi, obj in enumerate(sector.objects):
                ox, oy, oz = obj.points[0]
                bx, by, bz = obj.points[1], obj.points[2], obj.points[3]
                world = [(ox + bx[0] * x + by[0] * y + bz[0] * z,
                          oy + bx[1] * x + by[1] * y + bz[1] * z,
                          oz + bx[2] * x + by[2] * y + bz[2] * z)
                         for x, y, z in obj.vertices]
                n = len(world)
                for pi, poly in enumerate(obj.polys):
                    idx = poly.indices
                    if len(idx) < 3:
                        continue
                    for k in range(1, len(idx) - 1):
                        i0, i1, i2 = idx[0], idx[k], idx[k + 1]
                        if max(i0, i1, i2) >= n:
                            continue
                        t = Triangle()
                        t.a, t.b, t.c = world[i0], world[i1], world[i2]
                        e1, e2 = _v(t.b, t.a), _v(t.c, t.a)
                        face = _cross(e1, e2)
                        area2 = _len(face)
                        e_max = max(_len(e1), _len(e2), _len(_v(t.c, t.b)))
                        if not area2 > 1e-12 * max(1.0, e_max * e_max):
                            continue
                        t.normal = _mul(face, 1.0 / area2)
                        t.lo = tuple(min(p[k] for p in (t.a, t.b, t.c)) for k in range(3))
                        t.hi = tuple(max(p[k] for p in (t.a, t.b, t.c)) for k in range(3))
                        t.sector, t.object, t.poly = si, oi, pi
                        t.texture, t.material = poly.texture, poly.material
                        t.lightmap = poly.lightmap
                        self.tris.append(t)
            self.sector_ranges.append((begin, len(self.tris) - begin))

    def bounds(self):
        return (_box_min(self.tris), _box_max(self.tris))

    def sector_bounds(self, sector: int):
        begin, count = self.sector_ranges[sector]
        tris = self.tris[begin:begin + count]
        if not tris:
            return None
        return (_box_min(tris), _box_max(tris))

    def raycast(self, origin, direction, max_t=INF, sectors=None, want=None):
        d = _norm(direction)
        best, hit = max_t, None
        for index, t in enumerate(self.tris):
            if sectors is not None and t.sector not in sectors:
                continue
            if want is not None and t.surface() not in want:
                continue
            if not _aabb_hit(origin, d, t.lo, t.hi, best):
                continue
            tt = _ray_triangle(origin, d, t, best)
            if tt is None or tt > best:
                continue
            if hit is not None and not tt < best:
                continue
            best, hit = tt, (t, index)
        if hit is None:
            return None
        t, index = hit
        facing = _dot(t.normal, d)
        normal = _mul(t.normal, -1.0) if facing > 0.0 else t.normal
        return {"t": best, "point": _add(origin, _mul(d, best)),
                "normal": normal, "sector": t.sector, "object": t.object,
                "poly": t.poly, "texture": t.texture, "index": index,
                "front_face": facing < 0.0, "surface": t.surface()}

    def floor_at(self, x, y, z):
        hit = self.raycast((x, y, z), (0.0, 0.0, -1.0), want={"floor"})
        return None if hit is None else hit["point"][2]

    def overlap(self, box, want=None):
        lo, hi = box
        n = 0
        for t in self.tris:
            if want is not None and t.surface() not in want:
                continue
            if all(lo[k] <= t.hi[k] and t.lo[k] <= hi[k] for k in range(3)):
                n += 1
        return n


def _box_min(tris):
    return tuple(min(t.lo[k] for t in tris) for k in range(3))


def _box_max(tris):
    return tuple(max(t.hi[k] for t in tris) for k in range(3))


def _aabb_hit(origin, d, lo, hi, best):
    t_min, t_max = 0.0, best
    for k in range(3):
        inv = 1.0 / d[k] if d[k] != 0.0 else math.inf
        ta, tb = (lo[k] - origin[k]) * inv, (hi[k] - origin[k]) * inv
        if ta > tb:
            ta, tb = tb, ta
        t_min = max(t_min, ta)
        t_max = min(t_max, tb)
        if t_min > t_max:
            return False
    return True


def _ray_triangle(origin, d, t, max_t):
    e1, e2 = _v(t.b, t.a), _v(t.c, t.a)
    p = _cross(d, e2)
    det = _dot(e1, p)
    if abs(det) <= 1e-12:
        return None
    inv = 1.0 / det
    s = _v(origin, t.a)
    u = _dot(s, p) * inv
    if u < -BAR_EPS or u > 1.0 + BAR_EPS:
        return None
    q = _cross(s, e1)
    v = _dot(d, q) * inv
    if v < -BAR_EPS or u + v > 1.0 + BAR_EPS:
        return None
    tt = _dot(e2, q) * inv
    if not tt >= RAY_EPS or tt > max_t:
        return None
    return tt


def report(level_path: str, eye=EYE) -> dict:
    world = World(sbw.parse_file(level_path))
    split = {"floor": 0, "wall": 0, "ceiling": 0}
    for t in world.tris:
        split[t.surface()] += 1
    lo, hi = world.bounds()
    out = {
        "triangles": len(world.tris),
        "floors": split["floor"], "walls": split["wall"],
        "ceilings": split["ceiling"],
        "bounds": {"lo": lo, "hi": hi},
        "bounds_box": (lo, hi),
        "floor_at_eye": world.floor_at(*eye),
        "rays": {
            "-z": world.raycast(eye, (0, 0, -1)),
            "+x": world.raycast(eye, (1, 0, 0)),
            "-x": world.raycast(eye, (-1, 0, 0)),
            "+y": world.raycast(eye, (0, 1, 0)),
            "-y": world.raycast(eye, (0, -1, 0)),
        },
        "ceilings_only_down": world.raycast(eye, (0, 0, -1), want={"ceiling"}),
        "floors_only_down": world.raycast(eye, (0, 0, -1), want={"floor"}),
        "no_walls_plus_y": world.raycast(eye, (0, 1, 0), want={"floor", "ceiling"}),
        "overlap_all": world.overlap((lo, hi)),
        "overlap_walls": world.overlap((lo, hi), want={"wall"}),
        "sphere_down": world.raycast((eye[0], eye[1], SPHERE_Z), (0, 0, -1)),
    }
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("level", nargs="?", default="level0")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    path = os.path.join(args.game, args.level + ".sbw")
    if not os.path.exists(path):
        print("нет файла: " + path, file=sys.stderr)
        return 1
    info = report(path)
    if args.json:
        def enc(o):
            if isinstance(o, tuple):
                return [enc(v) for v in o]
            if isinstance(o, float) and not math.isfinite(o):
                return str(o)
            return o
        print(json.dumps(info, default=enc, indent=2, ensure_ascii=False))
        return 0
    print("%s: %d треугольников (пол/стена/потолок %d/%d/%d)"
          % (args.level, info["triangles"], info["floors"], info["walls"],
             info["ceilings"]))
    lo, hi = info["bounds"]["lo"], info["bounds"]["hi"]
    print("world AABB   lo = (%.6f, %.6f, %.6f)" % lo)
    print("             hi = (%.6f, %.6f, %.6f)" % hi)
    print("floor_at(eye) = %r" % (info["floor_at_eye"],))
    for name, hit in info["rays"].items():
        if hit is None:
            print("%-4s  miss" % name)
            continue
        print("%-4s  t = %.9f  point = (%.6f, %.6f, %.6f)  n = (%.12f, "
              "%.12f, %.12f)  sector %d object %d tex %d face=%s %s"
              % ((name, hit["t"]) + hit["point"] + hit["normal"]
                 + (hit["sector"], hit["object"], hit["texture"],
                    hit["front_face"], hit["surface"])))
    for key in ("ceilings_only_down", "floors_only_down", "no_walls_plus_y"):
        print("%-18s %s" % (key, "miss" if info[key] is None else
                            "t = %.9f" % info[key]["t"]))
    print("overlap: all %d, walls %d" % (info["overlap_all"], info["overlap_walls"]))
    down = info["sphere_down"]
    if down is not None:
        t = down["t"] - SPHERE_R
        print("sphere r=%g from z=%g: t = %.9f centre.z = %.9f point.z = %.9f "
              "sector %d" % (SPHERE_R, SPHERE_Z, t, SPHERE_Z - t,
                             down["point"][2], down["sector"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
