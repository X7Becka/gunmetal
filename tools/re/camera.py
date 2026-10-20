#!/usr/bin/env python3
"""Камера первого лица Gunmetal: углы игрока -> мировые оси и матрица вида.

Что доказано (адреса — виртуальные адреса ``gundx.exe`` v1.36, база ``0x400000``;
подробности и листинг — ``docs/CAMERA.md``):

* Ориентация строится одной функцией ``0x41e025(out1, out2, out3, a, b, c)``::

      R = Rz(-c) * Rx(-b) * Ry(a)          (столбцы = e1, e2, e3)

      e1 = ( cC*cA - sC*sB*sA, -sC*cA - cC*sB*sA, -cB*sA )
      e2 = ( sC*cB,             cC*cB,            -sB    )
      e3 = ( cC*sA + sC*sB*cA, -sC*sA + cC*sB*cA,  cB*cA )

  где ``A=a``, ``B=b``, ``C=c``.  Формулы сняты с ``0x41e025`` один-в-один
  (``fsin``/``fcos`` — ``0x480314``/``0x480954``) и проверены на совпадение с
  матричным произведением (см. ``--selftest``).

* Камера (``0x446590``) вызывает её как ``a = player+0x20`` (крен),
  ``b = player+0x18 + [0x81bea0]`` (тангаж + отдача), ``c = player+0x28``
  (рыскание).  В нуле все три вектора равны мировым осям:
  ``e1 = +X`` (вправо), ``e2 = +Y`` (вперёд), ``e3 = +Z`` (вверх), — поэтому
  мировые оси .sbw: X — вправо, Y — вперёд, Z — вверх.

* Рендер (``0x41928e`` и ``0x4409b8``) берёт ``e1`` как экранную X, ``-e3``
  как экранную Y (экранная Y растёт вниз, отсюда ``fchs`` в ``0x446915``..
  ``0x446942``) и ``e2`` как ось глубины.  Значит направление взгляда = ``e2``.

* Точка камеры: ``(player.x, player.y, player.z + player+0x2F50)``
  (``0x448d2b``..``0x448d3a``); ``player+0x2F50`` — высота глаза шасси.

* Проекция (``0x4466a6``..``0x446780``): масштабы ``+0x90 = zoom`` и
  ``+0x94 = zoom * 4H / (3W)``; при 320x200 это ``1.0`` и ``5/6``, при
  640x480 — ``1.0`` и ``1.0`` (см. :func:`projection`).

Использование::

    python3 tools/re/camera.py                     # все 36 уровней, слот 0
    python3 tools/re/camera.py level0 dm1 --json
    python3 tools/re/camera.py level1 --all-spawns
    python3 tools/re/camera.py --dm --slot 3
    python3 tools/re/camera.py level0 --pitch 15 --roll 5   # подмена углов
    python3 tools/re/camera.py --no-geom --json > out/camera.json
    python3 tools/re/camera.py --width 640 --height 480 --zoom 1
    python3 tools/re/camera.py --selftest

Только чтение: ничего не пишет в игру.  ``--json`` — в stdout.
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for p in (ROOT, HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import spawn as spawnmod            # noqa: E402  (tools/re/spawn.py)
from gm import sbw as sbwmod        # noqa: E402

DEFAULT_GAME = spawnmod.DEFAULT_GAME

#: Допуск на «вектор единичной длины» / ортонормальность.
EPS = 1e-9
#: Ближе этого старт считается «в стене» (эвристика spawn.py).
WALL_EPS = 1.0
#: Максимальная длина пробного луча, мировые единицы.
RAY_MAX = 100000.0

TAU = 2.0 * math.pi

#: Разрешение внутреннего растеризатора (0x83d944 = W, 0x83d630 = H).
PROJ_WIDTH = 320
PROJ_HEIGHT = 200
#: ``[0x4aa390]`` — 1.0 в игре, 0.25 в cutscene.
PROJ_ZOOM = 1.0


class CameraError(ValueError):
    """Некорректные входные данные (NaN/inf, нулевой вектор)."""


# --------------------------------------------------------------------------
# математика
# --------------------------------------------------------------------------

def _finite(*values) -> bool:
    return all(isinstance(v, (int, float)) and math.isfinite(v) for v in values)


def basis(roll: float, pitch: float, yaw: float):
    """Базис камеры ровно по формулам ``0x41e025`` (a=roll, b=pitch, c=yaw).

    Возвращает ``(e1, e2, e3)`` — столбцы ``R = Rz(-yaw) Rx(-pitch) Ry(roll)``:
    e1 — вправо, e2 — вперёд (направление взгляда), e3 — вверх.
    """
    if not _finite(roll, pitch, yaw):
        raise CameraError("углы должны быть конечными: %r %r %r"
                          % (roll, pitch, yaw))
    sA, cA = math.sin(roll), math.cos(roll)
    sB, cB = math.sin(pitch), math.cos(pitch)
    sC, cC = math.sin(yaw), math.cos(yaw)
    e1 = (cC * cA - sC * sB * sA,
          -sC * cA - cC * sB * sA,
          -cB * sA)
    e2 = (sC * cB,
          cC * cB,
          -sB)
    e3 = (cC * sA + sC * sB * cA,
          -sC * sA + cC * sB * cA,
          cB * cA)
    return e1, e2, e3


def _mm(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


def _rot(axis: str, ang: float):
    s, c = math.sin(ang), math.cos(ang)
    if axis == "x":
        return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == "y":
        return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def rotation_matrix(roll: float, pitch: float, yaw: float):
    """``Rz(-yaw) * Rx(-pitch) * Ry(roll)`` — то же, что :func:`basis`."""
    return _mm(_mm(_rot("z", -yaw), _rot("x", -pitch)), _rot("y", roll))


def dot(a, b) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm(a) -> float:
    return math.sqrt(dot(a, a))


def normalize(a, what="vector"):
    n = norm(a)
    if not math.isfinite(n) or n < EPS:
        raise CameraError("нулевой/нечисловой %s: %r" % (what, a))
    return (a[0] / n, a[1] / n, a[2] / n)


def view_matrix(eye, e1, e2, e3):
    """4x4 world->camera (x=вправо, y=вверх, z=вперёд), row-major.

    Экранный растеризатор движка использует строки ``e1``, ``-e3`` (вниз) и
    ``e2`` — см. ``docs/CAMERA.md`` §4.
    """
    if not _finite(*eye):
        raise CameraError("глаз не конечен: %r" % (eye,))
    return [
        [e1[0], e1[1], e1[2], -dot(e1, eye)],
        [e3[0], e3[1], e3[2], -dot(e3, eye)],
        [e2[0], e2[1], e2[2], -dot(e2, eye)],
        [0.0, 0.0, 0.0, 1.0],
    ]


def eye_from_spawn(pos, eye_height: float):
    """``camera_z = spawn_z + player[0x2F50]`` (``0x448d31``)."""
    if not _finite(*pos) or not _finite(eye_height):
        raise CameraError("позиция/высота глаза не конечны: %r %r"
                          % (pos, eye_height))
    return (float(pos[0]), float(pos[1]), float(pos[2]) + float(eye_height))


def projection(width=PROJ_WIDTH, height=PROJ_HEIGHT, zoom=PROJ_ZOOM):
    """Масштабы проекции из ``0x4466a6``..``0x446780``.

    ``+0x90 = zoom`` (f32), ``+0x94 = zoom * 4*H / (3*W)`` (f32); поля
    умножают ``e1`` и ``e3`` при выводе кадра (``0x440c1d``..``0x440c8f``).
    Явного угла FOV в данных нет — он выводится из этих масштабов.
    """
    if not _finite(width, height, zoom) or width <= 0 or height <= 0 or zoom <= 0:
        raise CameraError("размер/zoom должны быть положительными: %r %r %r"
                          % (width, height, zoom))
    focal_x = float(zoom)
    focal_y = float(zoom) * 4.0 * float(height) / (3.0 * float(width))
    return {
        "width": int(width),
        "height": int(height),
        "zoom": float(zoom),
        "aspect": float(width) / float(height),
        "focal_x": focal_x,
        "focal_y": focal_y,
        "focal_ratio": focal_y / focal_x,
    }


def frame_from_spawn(pos, angles, eye_height: float,
                     pitch=None, roll=None, yaw=None):
    """Полное состояние камеры для точки спавна.

    *angles* — ``(a0, a1, a2)`` из ``SET_*_START``: ``a0`` = тангаж (+0x18),
    ``a1`` = крен (+0x20), ``a2`` = рыскание (+0x28).  ``pitch``/``roll``/
    ``yaw`` (радианы) переопределяют соответствующее значение.
    """
    src_pitch, src_roll, src_yaw = (float(x) for x in angles[:3])
    pitch = src_pitch if pitch is None else float(pitch)
    roll = src_roll if roll is None else float(roll)
    yaw = src_yaw if yaw is None else float(yaw)
    e1, e2, e3 = basis(roll, pitch, yaw)
    eye = eye_from_spawn(pos, eye_height)
    return {
        "spawn": [float(pos[0]), float(pos[1]), float(pos[2])],
        "eye": list(eye),
        "eye_height": float(eye_height),
        "angles": {"pitch": pitch, "roll": roll, "yaw": yaw},
        "angles_source": {"pitch": src_pitch, "roll": src_roll, "yaw": src_yaw},
        "right": list(e1),
        "forward": list(e2),
        "up": list(e3),
        "matrix": view_matrix(eye, e1, e2, e3),
    }


# --------------------------------------------------------------------------
# проверки
# --------------------------------------------------------------------------

def check_frame(fr):
    """NaN/нулевые векторы, единичная длина, ортонормальность, det=+1."""
    errs = []
    for key in ("eye", "spawn"):
        if not all(math.isfinite(x) for x in fr[key]):
            errs.append("%s: NaN/inf" % key)
    for key in ("right", "forward", "up"):
        v = fr[key]
        n = norm(v)
        if not all(math.isfinite(x) for x in v) or n < EPS:
            errs.append("%s: нулевой/нечисловой" % key)
            continue
        if abs(n - 1.0) > 1e-9:
            errs.append("%s: |v| = %.12f" % (key, n))
    e1 = tuple(fr["right"])
    e2 = tuple(fr["forward"])
    e3 = tuple(fr["up"])
    for name, a, b in (("right·forward", e1, e2), ("right·up", e1, e3),
                       ("forward·up", e2, e3)):
        if abs(dot(a, b)) > 1e-9:
            errs.append("%s: %.3e" % (name, dot(a, b)))
    det = (e1[0] * (e2[1] * e3[2] - e2[2] * e3[1])
           - e1[1] * (e2[0] * e3[2] - e2[2] * e3[0])
           + e1[2] * (e2[0] * e3[1] - e2[1] * e3[0]))
    if not math.isfinite(det) or abs(det - 1.0) > 1e-9:
        errs.append("det = %.12f (ожидалось +1)" % det)
    m = fr["matrix"]
    if not all(math.isfinite(x) for row in m for x in row):
        errs.append("matrix: NaN/inf")
    return {"ok": not errs, "errors": errs, "det": det,
            "forward_norm": norm(tuple(fr["forward"]))}


# --------------------------------------------------------------------------
# геометрия .sbw (пробный луч + ближайшая стена)
# --------------------------------------------------------------------------

class Geometry:
    """Ленивая геометрия уровня: треугольники + сетка spawn.TriGrid."""

    def __init__(self, sbw_path):
        self.path = sbw_path
        world = sbwmod.parse_file(sbw_path)
        self.tris = spawnmod.world_triangles(world)
        self.grid = spawnmod.TriGrid(self.tris)
        self.aabb = self.grid.aabb()

    def inside_aabb(self, p) -> bool:
        if self.aabb is None:
            return False
        mn, mx = self.aabb
        return all(mn[k] <= p[k] <= mx[k] for k in range(3))

    def nearest_wall(self, p):
        return self.grid.nearest(p, walls_only=True)

    def ray(self, origin, direction, max_dist=RAY_MAX):
        """Ближайшее пересечение с треугольником: ``dict`` или ``None``.

        Мёллер—Трумбур; нормаль не нормируется на площадь дважды.
        """
        o = origin
        d = direction
        best = None
        for tri in self.tris:
            a, b, c = tri
            e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
            h = (d[1] * e2[2] - d[2] * e2[1],
                 d[2] * e2[0] - d[0] * e2[2],
                 d[0] * e2[1] - d[1] * e2[0])
            det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2]
            if abs(det) < 1e-12:
                continue
            inv = 1.0 / det
            s = (o[0] - a[0], o[1] - a[1], o[2] - a[2])
            u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2])
            if u < 0.0 or u > 1.0:
                continue
            q = (s[1] * e1[2] - s[2] * e1[1],
                 s[2] * e1[0] - s[0] * e1[2],
                 s[0] * e1[1] - s[1] * e1[0])
            v = inv * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2])
            if v < 0.0 or u + v > 1.0:
                continue
            t = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2])
            if t <= 1e-9 or t > max_dist:
                continue
            if best is not None and t >= best["t"]:
                continue
            n = (e1[1] * e2[2] - e1[2] * e2[1],
                 e1[2] * e2[0] - e1[0] * e2[2],
                 e1[0] * e2[1] - e1[1] * e2[0])
            ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
            n = (n[0] / ln, n[1] / ln, n[2] / ln)
            best = {
                "t": t,
                "point": [o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t],
                "normal": list(n),
                #: |n_z| < 0.8 — «стена», иначе пол/потолок/скат
                "vertical": abs(n[2]) < 0.8,
            }
        return best

    def probe(self, fr):
        """Проверка старта: AABB, ближайшая стена, луч вперёд/назад/вниз."""
        eye = tuple(fr["eye"])
        fwd = tuple(fr["forward"])
        back = (-fwd[0], -fwd[1], -fwd[2])
        wall = self.nearest_wall(eye)
        hit_f = self.ray(eye, fwd)
        hit_b = self.ray(eye, back)
        floor = self.grid.floor(eye)
        return {
            "inside_world_aabb": self.inside_aabb(eye),
            "nearest_wall": None if wall is None else round(wall, 1),
            "in_wall": wall is not None and wall < WALL_EPS,
            "forward_hit": None if hit_f is None else round(hit_f["t"], 1),
            "forward_hit_vertical": None if hit_f is None else hit_f["vertical"],
            "backward_hit": None if hit_b is None else round(hit_b["t"], 1),
            "floor_z": None if floor is None else round(floor, 1),
            "floor_gap": None if floor is None else round(eye[2] - floor, 1),
        }


_GEOM_CACHE = {}


def geometry(path):
    if path not in _GEOM_CACHE:
        _GEOM_CACHE[path] = Geometry(path)
    return _GEOM_CACHE[path]


# --------------------------------------------------------------------------
# уровни
# --------------------------------------------------------------------------

def level_names(game):
    out = []
    for path in sorted(glob.glob(os.path.join(game, "*.sbt"))):
        stem = os.path.splitext(os.path.basename(path))[0]
        if os.path.exists(os.path.join(game, stem + ".sbw")):
            out.append(stem)
    return out


def resolve(name, game):
    """Имя уровня или путь к .sbt -> путь."""
    if os.path.isfile(name):
        return name
    stem = os.path.splitext(os.path.basename(name))[0]
    path = os.path.join(game, stem + ".sbt")
    if not os.path.exists(path):
        raise SystemExit("нет уровня: %s" % name)
    return path


def camera_level(path, game, slot=0, deathmatch=False, all_spawns=False,
                 eye=None, chassis=spawnmod.DEFAULT_CHASSIS, geom=True,
                 pitch=None, roll=None, yaw=None, allow_empty=False):
    d = spawnmod.level_data(path, game, geom=False, chassis=chassis, eye=eye)
    points = d["deathmatch"] if deathmatch else d["campaign"]
    if not points:
        if allow_empty:
            return None
        raise SystemExit("%s: нет точек %s"
                         % (d["level"], "DM" if deathmatch else "кампании"))
    if all_spawns:
        chosen = points
    else:
        chosen = [p for p in points if p["index"] == slot]
        if not chosen:
            raise SystemExit("%s: нет слота %d (есть %s)"
                             % (d["level"], slot,
                                ", ".join(str(p["index"]) for p in points)))

    g = None
    if geom and d.get("sbw") and os.path.exists(d["sbw"]):
        g = geometry(d["sbw"])

    out = []
    for p in chosen:
        eye_h = p["eye_height"]
        fr = frame_from_spawn(p["pos"], p["angles"], eye_h,
                              pitch=pitch, roll=roll, yaw=yaw)
        fr["index"] = p["index"]
        fr["kind"] = "deathmatch" if deathmatch else "campaign"
        fr["yaw_deg"] = math.degrees(fr["angles"]["yaw"]) % 360.0
        if fr["yaw_deg"] > 180.0:
            fr["yaw_deg"] -= 360.0
        fr["pitch_deg"] = math.degrees(fr["angles"]["pitch"])
        fr["roll_deg"] = math.degrees(fr["angles"]["roll"])
        fr["source"] = p["source"]
        fr["checks"] = check_frame(fr)
        if g is not None:
            fr["geometry"] = g.probe(fr)
        else:
            fr["geometry"] = None
        out.append(fr)

    return {
        "level": d["level"],
        "sbt": d["sbt"],
        "sbw": d.get("sbw"),
        "chassis": chassis,
        "eye_default": d["eye_default"],
        "campaign": d["counts"]["campaign"],
        "deathmatch": d["counts"]["deathmatch"],
        "spawns": out,
    }


# --------------------------------------------------------------------------
# вывод
# --------------------------------------------------------------------------

def _v(v):
    return "(%+.6f, %+.6f, %+.6f)" % (v[0], v[1], v[2])


def print_level(lv):
    print("%s  (кампания %d, DM %d, шасси %s, глаз %g)"
          % (lv["level"], lv["campaign"], lv["deathmatch"], lv["chassis"],
             lv["eye_default"]))
    for fr in lv["spawns"]:
        src = fr["source"]
        print("  [%d] %s (%s, op %d)" % (fr["index"], fr["kind"],
                                        src["command"], src["opcode"]))
        print("      spawn   %s  yaw %+.3f°  pitch %+.3f°  roll %+.3f°"
              % (_v(fr["spawn"]), fr["yaw_deg"], fr["pitch_deg"],
                 fr["roll_deg"]))
        print("      eye     %s   (z + player+0x2F50 = %g)"
              % (_v(fr["eye"]), fr["eye_height"]))
        print("      right   %s" % _v(fr["right"]))
        print("      forward %s" % _v(fr["forward"]))
        print("      up      %s" % _v(fr["up"]))
        print("      view (world -> camera, x=right y=up z=forward):")
        for row in fr["matrix"]:
            print("        [%+.6f %+.6f %+.6f %+.6f]" % tuple(row))
        ch = fr["checks"]
        print("      math: %s  det %+.9f  |forward| %.9f"
              % ("ok" if ch["ok"] else "ОШИБКА", ch["det"],
                 ch["forward_norm"]))
        for e in ch["errors"]:
            print("        ! %s" % e)
        gm = fr["geometry"]
        if gm:
            print("      geom: aabb=%s nearest_wall=%s in_wall=%s "
                  "forward_hit=%s%s backward_hit=%s floor_gap=%s"
                  % (gm["inside_world_aabb"], gm["nearest_wall"],
                     gm["in_wall"], gm["forward_hit"],
                     "" if gm["forward_hit_vertical"] is None
                     else ("(стена)" if gm["forward_hit_vertical"]
                           else "(не стена)"),
                     gm["backward_hit"], gm["floor_gap"]))
    print()


# --------------------------------------------------------------------------
# самопроверка математики
# --------------------------------------------------------------------------

def selftest(trials=2000, seed=1):
    """Сверить формулы 0x41e025 с матричным произведением и свойствами."""
    import random
    rnd = random.Random(seed)
    worst = 0.0
    worst_det = 0.0
    for _ in range(trials):
        roll = rnd.uniform(-TAU, TAU)
        pitch = rnd.uniform(-math.pi / 2, math.pi / 2)
        yaw = rnd.uniform(-TAU, TAU)
        e1, e2, e3 = basis(roll, pitch, yaw)
        R = rotation_matrix(roll, pitch, yaw)
        for col, e in zip(range(3), (e1, e2, e3)):
            for row in range(3):
                worst = max(worst, abs(R[row][col] - e[row]))
        for a, b in ((e1, e2), (e1, e3), (e2, e3)):
            worst_det = max(worst_det, abs(dot(a, b)))
        det = (e1[0] * (e2[1] * e3[2] - e2[2] * e3[1])
               - e1[1] * (e2[0] * e3[2] - e2[2] * e3[0])
               + e1[2] * (e2[0] * e3[1] - e2[1] * e3[0]))
        if abs(det - 1.0) > 1e-12:
            raise CameraError("det != 1: %.15f" % det)
    # частные случаи
    e1, e2, e3 = basis(0.0, 0.0, 0.0)
    assert e1 == (1.0, 0.0, 0.0) and e2 == (0.0, 1.0, 0.0) and e3 == (0.0, 0.0, 1.0)
    e1, e2, e3 = basis(0.0, 0.0, math.pi / 2)
    assert abs(e2[0] - 1.0) < 1e-12 and abs(e2[1]) < 1e-12  # yaw +90° -> +X
    e1, e2, e3 = basis(0.0, math.pi / 2, 0.0)
    assert abs(e2[2] + 1.0) < 1e-12                          # pitch +90° -> вниз
    # защита от мусора
    for bad in (float("nan"), float("inf")):
        try:
            basis(0.0, 0.0, bad)
        except CameraError:
            pass
        else:
            raise CameraError("NaN/inf не отловлен")
    try:
        normalize((0.0, 0.0, 0.0))
    except CameraError:
        pass
    else:
        raise CameraError("нулевой вектор не отловлен")
    p = projection(320, 200, 1.0)
    assert abs(p["focal_x"] - 1.0) < 1e-15
    assert abs(p["focal_y"] - 5.0 / 6.0) < 1e-15
    p = projection(640, 480, 1.0)
    assert abs(p["focal_x"] - 1.0) < 1e-15
    assert abs(p["focal_y"] - 1.0) < 1e-15
    p = projection(320, 200, 0.25)
    assert abs(p["focal_y"] - 5.0 / 24.0) < 1e-15
    for bad in ((0, 200, 1.0), (320, 0, 1.0), (320, 200, 0.0),
                (float("nan"), 200, 1.0)):
        try:
            projection(*bad)
        except CameraError:
            pass
        else:
            raise CameraError("плохая проекция не отловлена: %r" % (bad,))
    print("selftest: ok  (%d наборов углов; max |R - basis| = %.3e, "
          "max |dot| = %.3e)" % (trials, worst, worst_det))
    return 0


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="камера первого лица Gunmetal: оси, матрица вида, проверки")
    ap.add_argument("levels", nargs="*", help="имена уровней или .sbt")
    ap.add_argument("--game", default=DEFAULT_GAME, help="каталог игры")
    ap.add_argument("--slot", type=int, default=0, help="слот игрока (0..7)")
    ap.add_argument("--dm", action="store_true",
                    help="SET_DEATHMATCH_START вместо SET_PLAYER_START")
    ap.add_argument("--all-spawns", action="store_true",
                    help="все точки выбранного типа, не только слот")
    ap.add_argument("--eye", type=float, default=None,
                    help="высота глаза вручную (по умолчанию player+0x2F50)")
    ap.add_argument("--chassis", default=spawnmod.DEFAULT_CHASSIS,
                    help="шасси для player+0x2F50 (LIGHT/MEDIUM/...)")
    ap.add_argument("--width", type=int, default=PROJ_WIDTH,
                    help="ширина растеризатора для проекции (по умолчанию 320)")
    ap.add_argument("--height", type=int, default=PROJ_HEIGHT,
                    help="высота растеризатора для проекции (по умолчанию 200)")
    ap.add_argument("--zoom", type=float, default=PROJ_ZOOM,
                    help="zoom из [0x4aa390] (1.0 игра, 0.25 cutscene)")
    ap.add_argument("--pitch", type=float, default=None,
                    help="тангаж в градусах (переопределить скрипт)")
    ap.add_argument("--roll", type=float, default=None,
                    help="крен в градусах (переопределить скрипт)")
    ap.add_argument("--yaw", type=float, default=None,
                    help="рыскание в градусах (переопределить скрипт)")
    ap.add_argument("--no-geom", action="store_true",
                    help="не читать .sbw и не проверять геометрию")
    ap.add_argument("--strict", action="store_true",
                    help="ненулевой код возврата также при in_wall")
    ap.add_argument("--json", action="store_true", help="JSON в stdout")
    ap.add_argument("--selftest", action="store_true",
                    help="проверить формулы на случайных углах и выйти")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    names = args.levels or level_names(args.game)
    deg = math.pi / 180.0
    try:
        proj = projection(args.width, args.height, args.zoom)
    except CameraError as exc:
        ap.error(str(exc))
    levels = []
    for name in names:
        lv = camera_level(
            resolve(name, args.game), args.game, slot=args.slot,
            deathmatch=args.dm, all_spawns=args.all_spawns, eye=args.eye,
            chassis=args.chassis, geom=not args.no_geom,
            pitch=None if args.pitch is None else args.pitch * deg,
            roll=None if args.roll is None else args.roll * deg,
            yaw=None if args.yaw is None else args.yaw * deg,
            allow_empty=not args.levels)
        if lv is not None:
            levels.append(lv)

    total = sum(len(lv["spawns"]) for lv in levels)
    bad = [lv["level"] for lv in levels
           if any(not fr["checks"]["ok"] for fr in lv["spawns"])]
    in_wall = [(lv["level"], fr["index"]) for lv in levels
               for fr in lv["spawns"]
               if fr["geometry"] and fr["geometry"]["in_wall"]]
    outside = [(lv["level"], fr["index"]) for lv in levels
               for fr in lv["spawns"]
               if fr["geometry"] and not fr["geometry"]["inside_world_aabb"]]

    if args.json:
        json.dump({"game": args.game,
                   "camera": {
                       "convention": "Z up, Y forward at yaw=0; "
                                     "forward = e2 = (sin(yaw)cos(pitch), "
                                     "cos(yaw)cos(pitch), -sin(pitch))",
                       "eye": "spawn + (0,0,player+0x2F50)",
                       "angles": "a0=+0x18 pitch, a1=+0x20 roll, a2=+0x28 yaw",
                       "projection": proj,
                   },
                   "levels": levels},
                  sys.stdout, ensure_ascii=False, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print("проекция: %dx%d, zoom %g -> focal_x %g, focal_y %g "
              "(focal_y/focal_x = %g)"
              % (proj["width"], proj["height"], proj["zoom"],
                 proj["focal_x"], proj["focal_y"],
                 proj["focal_y"] / proj["focal_x"]))
        for lv in levels:
            print_level(lv)
        print("итог: уровней %d, точек %d, ошибок математики %d"
              % (len(levels), total, len(bad)))
        if bad:
            print("  ошибки: %s" % ", ".join(bad))
        if not args.no_geom:
            print("  стартов в стене (<%.1f): %d %s"
                  % (WALL_EPS, len(in_wall),
                     " ".join("%s[%d]" % t for t in in_wall) or "-"))
            print("  стартов вне AABB .sbw: %d" % len(outside))

    if bad:
        return 1
    if args.strict and in_wall:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
