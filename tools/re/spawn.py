#!/usr/bin/env python3
"""Спавн игрока и параметры первого лица для уровней Gunmetal.

Что доказано (подробности и адреса — ``docs/PLAYER.md``):

* Точка старта задаётся командами скрипта ``SET_PLAYER_START`` (op **169**,
  сигнатура ``LLLLAAA``) и ``SET_DEATHMATCH_START`` (op **168**).  Обработчики
  ``0x45c270`` / ``0x45c2bc`` вызывают ``0x450880`` / ``0x4508d0``, которые
  раскладывают операнды по массивам на 8 слотов:
  ``0x5a9460/80/a0`` (x/y/z, int) + ``0x5a94c0/95a8/96a8`` (три double) для
  кампании, ``0x5a9540/60/80`` + ``0x5a9500/9668/96e8`` для DM.
* При (ре)старте уровня ``0x452730`` копирует выбранный слот в структуру игрока
  (``0x819040``): ``fild`` int'ов -> ``fstp`` в ``+0x00/+0x08/+0x10`` (там
  лежат double'ы, 0x819040/48/50), три double-угла -> ``+0x18/+0x20/+0x28``
  (0x819058/60/68).  Выбор слота: старт уровня ``0x453215`` вызывает
  ``0x452730(player, arg2, 0)``, респавн ``0x452698`` — с arg3=1.  При arg3=0
  и ``0x4421c0() == 0`` (флаг ``0x83d2c8``) берётся ``SET_PLAYER_START`` с
  индексом слота игрока (0 в одиночной игре), иначе — ``SET_DEATHMATCH_START``
  с тем же индексом.  При arg3=1 в DM (``0x4421c0() != 0``) движок перебирает
  слоты и берёт самый дальний от других игроков (цикл ``0x452969``..
  ``0x452a79``); в кампании по-прежнему ``SET_PLAYER_START[slot]``.
* Камера первого лица: ``0x448d2b``..``0x448d3a`` берёт z игрока
  (``0x819050``), добавляет поле ``player+0x2F50`` (``0x81bf90``); в режиме
  camera mode 1 та же пара складывается в ``0x5a4718`` (``0x448da2``..
  ``0x448ddf``).  Третья угловая величина ``player+0x28`` (``0x819068``)
  грузится в ``0x448dd1`` и уходит в построение видовой матрицы
  (``0x448e3c`` -> ``0x44a020``); первые две у всех 391 записи равны 0.
  ``player+0x2F50`` — «высота камеры» шасси: копируется из записи
  ``DEFINE_PLAYER_TYPES`` (op 172), в Python-разборе это ``vals[9]``
  (движок хранит double по адресу ``0x799620``);
  у LIGHT 10.0, MEDIUM/STEALTH 11.0, HEAVY 12.0, без шасси — 16.0 (``0x47aa87``).

Инструмент ничего не пишет в игру: читает ``.sbt``/``.sbw`` и печатает
точку(и) спавна, yaw, высоту камеры, источник и проверку по геометрии.

Использование::

    python3 tools/re/spawn.py                      # все 36 уровней, таблица
    python3 tools/re/spawn.py level1 dm2 --json    # выбранные уровни, JSON
    python3 tools/re/spawn.py level0 --no-geom     # без проверки по .sbw
    python3 tools/re/spawn.py --chassis "HEAVY CHASSIS"   # другая высота глаза
    python3 tools/re/spawn.py --eye 12.5           # высота камеры вручную

Проверка по ``.sbw``: точка внутри AABB мира, расстояние до ближайшей
поверхности (``in_wall`` при < 1.0), высота пола под точкой (``floor_gap``).
Многие стартовые точки лежат **вне** геометрии ``.sbw``: это интерьеры,
собранные из ``.sob``-моделей, которые скрипт ставит через ``ADD_THING``;
поэтому дополнительно печатается ближайший объект сцены (location/thing/
item/switch/door) — он обычно в десятках единиц от спавна.
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm                   # noqa: E402
from gm import scene as scenemod       # noqa: E402
from gm import sbw as sbwmod           # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: opcode -> (имя, обработчик (таблица 0x45c500, вход = op-1), функция-хранилище)
OPS = {
    169: ("SET_PLAYER_START", 0x45C270, 0x450880,
          ("0x5a9460", "0x5a9480", "0x5a94a0"),
          ("0x5a94c0", "0x5a95a8", "0x5a96a8")),
    168: ("SET_DEATHMATCH_START", 0x45C2BC, 0x4508D0,
          ("0x5a9540", "0x5a9560", "0x5a9580"),
          ("0x5a9500", "0x5a9668", "0x5a96e8")),
}

#: поле структуры игрока, куда ложится int-тройка (x/y/z) и double-тройка
PLAYER_FIELDS = ("+0x00/+0x08/+0x10", "+0x18/+0x20/+0x28")
PLAYER_BASE = 0x819040
EYE_FIELD = "+0x2F50"          # 0x81bf90 - 0x819040
EYE_FALLBACK = 16.0            # 0x47aa87, если шасси ещё не выбрано
DEFAULT_CHASSIS = "LIGHT CHASSIS"   # подтверждено сейвом LEVEL0 (eye = 10.0)

#: поля записи DEFINE_PLAYER_TYPES: имя -> индекс double в разобранной записи
CHASSIS_EYE_VAL = 9


def _s32(x: int) -> int:
    """4-байтовый операнд как signed int (движок читает их через fild)."""
    return x - 0x100000000 if x >= 0x80000000 else x


def _walk(insns):
    for ins in insns:
        yield ins
        if isinstance(getattr(ins, "body", None), list):
            yield from _walk(ins.body)


def _script(path):
    f = sbtmod.parse_file(path)
    recs = f.records_of(11)
    if not recs:
        raise ValueError("no SCRIPT record")
    return f, recs[0].payload


def chassis_eyes(f) -> dict:
    """``{имя шасси: высота камеры}`` из всех блоков DEFINE_PLAYER_TYPES."""
    out = {}
    for ins in _walk(sbtvm.decode(f.records_of(11)[0].payload.code)[0]):
        if ins.name != "DEFINE_PLAYER_TYPES":
            continue
        for strings, vals in ins.data:
            if len(vals) > CHASSIS_EYE_VAL:
                out.setdefault(strings[0].strip(), float(vals[CHASSIS_EYE_VAL]))
    return out


def _spawns(script, op):
    """Все записи команды *op* как ``(index, pos, angles, offset)``."""
    out = []
    insns, _ = sbtvm.decode(script.code)
    for ins in _walk(insns):
        if getattr(ins, "op", None) != op:
            continue
        a = ins.args
        if len(a) < 7:
            continue
        pos = tuple(_s32(a[k]) for k in (1, 2, 3))
        angles = tuple(float(a[k]) for k in (4, 5, 6))
        out.append((_s32(a[0]), pos, angles, ins.off))
    out.sort(key=lambda r: r[0])
    return out


# --- геометрия .sbw ---------------------------------------------------------

def world_triangles(w):
    """Треугольники уровня в мировом кадре (origin + basis * local)."""
    tris = []
    for sect in w.sectors:
        for obj in sect.objects:
            vs = obj.world_vertices()
            for poly in obj.polys:
                idx = poly.indices
                if len(idx) < 3:
                    continue
                a = vs[idx[0]]
                for k in range(1, len(idx) - 1):
                    tris.append((a, vs[idx[k]], vs[idx[k + 1]]))
    return tris


class TriGrid:
    """XY-сетка треугольников: ближайшая поверхность и луч вниз."""

    def __init__(self, tris, cell=512.0):
        self.tris = tris
        self.cell = cell
        self.cells = {}
        self.large = []
        #: |nz| < 0.8 -> поверхность считается стеной, а не полом/потолком
        self.vertical = []
        for a, b, c in tris:
            ab = _sub(b, a)
            ac = _sub(c, a)
            n = (ab[1] * ac[2] - ab[2] * ac[1],
                 ab[2] * ac[0] - ab[0] * ac[2],
                 ab[0] * ac[1] - ab[1] * ac[0])
            ln = math.sqrt(_dot(n, n))
            self.vertical.append(ln > 0 and abs(n[2] / ln) < 0.8)
        for i, (a, b, c) in enumerate(tris):
            x0 = min(a[0], b[0], c[0])
            x1 = max(a[0], b[0], c[0])
            y0 = min(a[1], b[1], c[1])
            y1 = max(a[1], b[1], c[1])
            cx0, cx1 = int(math.floor(x0 / cell)), int(math.floor(x1 / cell))
            cy0, cy1 = int(math.floor(y0 / cell)), int(math.floor(y1 / cell))
            if (cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 256:
                self.large.append(i)
                continue
            for cx in range(cx0, cx1 + 1):
                for cy in range(cy0, cy1 + 1):
                    self.cells.setdefault((cx, cy), []).append(i)

    def _cell_tris(self, x, y):
        idx = self.cells.get((int(math.floor(x / self.cell)),
                              int(math.floor(y / self.cell))), ())
        return list(idx) + self.large

    def nearest(self, p, max_ring=6, walls_only=False):
        """Расстояние до ближайшего треугольника или None (дальше max_ring*cell).

        С ``walls_only`` учитываются только «вертикальные» треугольники
        (``|n_z| < 0.8``): пол/потолок не делает точку «в стене».
        """
        best = None
        for ring in range(max_ring + 1):
            if best is not None and best <= ring * self.cell:
                break
            cx = int(math.floor(p[0] / self.cell))
            cy = int(math.floor(p[1] / self.cell))
            seen = set()
            for dx in range(-ring, ring + 1):
                for dy in range(-ring, ring + 1):
                    if max(abs(dx), abs(dy)) != ring:
                        continue
                    for i in self.cells.get((cx + dx, cy + dy), ()):
                        if i in seen:
                            continue
                        seen.add(i)
                        if walls_only and not self.vertical[i]:
                            continue
                        a, b, c = self.tris[i]
                        d = point_tri_distance(p, a, b, c)
                        if best is None or d < best:
                            best = d
            if ring == 0:
                for i in self.large:
                    if walls_only and not self.vertical[i]:
                        continue
                    a, b, c = self.tris[i]
                    d = point_tri_distance(p, a, b, c)
                    if best is None or d < best:
                        best = d
        return best

    def floor(self, p):
        """z ближайшей поверхности под точкой (луч вниз) или None."""
        x, y, z = p
        best = None
        for i in self._cell_tris(x, y):
            a, b, c = self.tris[i]
            x1, y1, z1 = a
            x2, y2, z2 = b
            x3, y3, z3 = c
            det = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
            if abs(det) < 1e-12:
                continue
            l1 = ((y2 - y3) * (x - x3) + (x3 - x2) * (y - y3)) / det
            l2 = ((y3 - y1) * (x - x3) + (x1 - x3) * (y - y3)) / det
            l3 = 1.0 - l1 - l2
            if l1 < -1e-6 or l2 < -1e-6 or l3 < -1e-6:
                continue
            zz = l1 * z1 + l2 * z2 + l3 * z3
            if zz <= z + 1e-3 and (best is None or zz > best):
                best = zz
        return best

    def aabb(self):
        xs = [v for t in self.tris for v in t]
        return ([min(v[k] for v in xs) for k in range(3)],
                [max(v[k] for v in xs) for k in range(3)])


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _norm(a):
    return math.sqrt(_dot(a, a))


def point_tri_distance(p, a, b, c):
    """Кратчайшее расстояние точка-треугольник (Ericson, RTCD 5.1.5)."""
    ab = _sub(b, a)
    ac = _sub(c, a)
    ap = _sub(p, a)
    d1 = _dot(ab, ap)
    d2 = _dot(ac, ap)
    if d1 <= 0.0 and d2 <= 0.0:
        return _norm(ap)
    bp = _sub(p, b)
    d3 = _dot(ab, bp)
    d4 = _dot(ac, bp)
    if d3 >= 0.0 and d4 <= d3:
        return _norm(bp)
    vc = d1 * d4 - d3 * d2
    if vc <= 0.0 and d1 >= 0.0 and d3 <= 0.0:
        v = d1 / (d1 - d3)
        return _norm((p[0] - (a[0] + v * ab[0]), p[1] - (a[1] + v * ab[1]),
                      p[2] - (a[2] + v * ab[2])))
    cp = _sub(p, c)
    d5 = _dot(ab, cp)
    d6 = _dot(ac, cp)
    if d6 >= 0.0 and d5 <= d6:
        return _norm(cp)
    vb = d5 * d2 - d1 * d6
    if vb <= 0.0 and d2 >= 0.0 and d6 <= 0.0:
        w = d2 / (d2 - d6)
        return _norm((p[0] - (a[0] + w * ac[0]), p[1] - (a[1] + w * ac[1]),
                      p[2] - (a[2] + w * ac[2])))
    va = d3 * d6 - d5 * d4
    if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return _norm((p[0] - (b[0] + w * (c[0] - b[0])),
                      p[1] - (b[1] + w * (c[1] - b[1])),
                      p[2] - (b[2] + w * (c[2] - b[2]))))
    denom = 1.0 / (va + vb + vc)
    v = vb * denom
    w = vc * denom
    q = (a[0] + ab[0] * v + ac[0] * w,
         a[1] + ab[1] * v + ac[1] * w,
         a[2] + ab[2] * v + ac[2] * w)
    return _norm(_sub(p, q))


def nearest_scene_object(scene, pos):
    """Ближайший объект, поставленный скриптом (для спавнов вне .sbw)."""
    cands = []
    for x in scene.locations:
        cands.append(("location", x.pos))
    for x in scene.things:
        if x.pos is not None:
            cands.append(("thing", x.pos))
    for x in scene.items:
        cands.append(("item", x.pos))
    for x in scene.switches:
        if x.absolute:
            cands.append(("switch", x.pos))
    for x in scene.doors:
        if len(x.pos) == 3:
            cands.append(("door", x.pos))
    if not cands:
        return None
    kind, p = min(cands, key=lambda c: sum((c[1][k] - pos[k]) ** 2 for k in range(3)))
    d = math.sqrt(sum((p[k] - pos[k]) ** 2 for k in range(3)))
    return {"kind": kind, "pos": [int(v) for v in p], "distance": round(d, 1)}


def level_data(path, game, geom=True, chassis=DEFAULT_CHASSIS, eye=None):
    f, script = _script(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    sbw_path = os.path.join(game, stem + ".sbw")
    eyes = chassis_eyes(f)
    if eye is None:
        eye = eyes.get(chassis, EYE_FALLBACK)

    grid = None
    aabb = None
    if geom and os.path.exists(sbw_path):
        grid = TriGrid(world_triangles(sbwmod.parse_file(sbw_path)))
        aabb = grid.aabb()

    screen = None
    try:
        screen = scenemod.extract(script)
    except Exception:                     # noqa: BLE001 - сцена не критична
        screen = None

    def one(op, idx, pos, angles, off):
        pitch, roll, yaw = angles
        #: градусы в (-180, 180]; сырое значение остаётся в ``yaw``
        yaw_deg = math.degrees(yaw) % 360.0
        if yaw_deg > 180.0:
            yaw_deg -= 360.0
        g = {"inside_world_aabb": None, "nearest_surface": None,
             "nearest_wall": None, "floor_z": None, "floor_gap": None,
             "in_wall": None}
        if grid is not None:
            mn, mx = aabb
            g["inside_world_aabb"] = all(mn[k] <= pos[k] <= mx[k] for k in range(3))
            d = grid.nearest(pos)
            g["nearest_surface"] = None if d is None else round(d, 1)
            w = grid.nearest(pos, walls_only=True)
            g["nearest_wall"] = None if w is None else round(w, 1)
            g["in_wall"] = (w is not None and w < 1.0)
            fl = grid.floor(pos)
            if fl is not None:
                g["floor_z"] = round(fl, 1)
                g["floor_gap"] = round(pos[2] - fl, 1)
        d = {"index": idx, "pos": list(pos), "yaw": yaw,
             "yaw_deg": round(yaw_deg, 1),
             "angles": [pitch, roll, yaw],
             "camera": [pos[0], pos[1], round(pos[2] + eye, 3)],
             "eye_height": eye, "eye_source": "player+0x2F50",
             "gravity": g,
             "source": {
                 "command": OPS[op][0], "opcode": op,
                 "script_offset": off, "handler": hex(OPS[op][1]),
                 "store": hex(OPS[op][2]),
                 "int_arrays": list(OPS[op][3]),
                 "double_arrays": list(OPS[op][4]),
                 "player_fields": list(PLAYER_FIELDS),
                 "evidence": "0x452b8f..0x452bfb (copy to 0x819040)",
             }}
        if screen is not None and geom:
            d["nearest_scene_object"] = nearest_scene_object(screen, pos)
        return d

    camp = [one(169, i, p, a, o) for i, p, a, o in _spawns(script, 169)]
    dm = [one(168, i, p, a, o) for i, p, a, o in _spawns(script, 168)]
    return {
        "level": stem,
        "sbt": path,
        "sbw": sbw_path if os.path.exists(sbw_path) else None,
        "chassis_eye": {k: v for k, v in sorted(eyes.items())},
        "campaign": camp,
        "deathmatch": dm,
        "counts": {"campaign": len(camp), "deathmatch": len(dm)},
        "chassis": chassis,
        "eye_default": eye,
        "errors": [],
    }


# --- вывод ------------------------------------------------------------------

def print_level(d):
    print("%s" % d["level"])
    print("  шасси / высота камеры: %s%s" % (
        ", ".join("%s=%g" % (k, v) for k, v in d["chassis_eye"].items()) or "-",
        " (используется %s)" % d["chassis"]))
    for kind, title in (("campaign", "кампания (слот игрока 0)"),
                        ("deathmatch", "DM (слот i; на респавне движок ищет самый дальний)")):
        rows = d[kind]
        if not rows:
            print("  %s: нет" % title)
            continue
        print("  %s: %d" % (title, len(rows)))
        for r in rows:
            g = r["gravity"]
            src = r["source"]
            print("    [%d] pos (%d, %d, %d)  yaw %+.4f rad (%+.1f°)  углы1/2 %g/%g" % (
                r["index"], r["pos"][0], r["pos"][1], r["pos"][2],
                r["yaw"], r["yaw_deg"], r["angles"][0], r["angles"][1]))
            print("        камера z %.1f (глаз +%g, поле %s)" % (
                r["camera"][2], r["eye_height"], r["eye_source"]))
            if g["inside_world_aabb"] is not None:
                extra = ""
                if g["nearest_surface"] is not None:
                    extra += " поверхность %.1f" % g["nearest_surface"]
                if g["nearest_wall"] is not None:
                    extra += " стена %.1f" % g["nearest_wall"]
                if g["floor_gap"] is not None:
                    extra += " пол %.1f" % g["floor_gap"]
                print("        .sbw: AABB=%s%s%s" % (
                    "да" if g["inside_world_aabb"] else "нет",
                    extra, "  В СТЕНЕ" if g["in_wall"] else ""))
            obj = r.get("nearest_scene_object")
            if obj:
                print("        ближайший объект сцены: %s %s, %.1f" % (
                    obj["kind"], tuple(obj["pos"]), obj["distance"]))
            print("        источник: %s op%d @ script+0x%x (handler %s, store %s)" % (
                src["command"], src["opcode"], src["script_offset"],
                src["handler"], src["store"]))
    print()


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Спавн игрока и первое лицо (SET_PLAYER_START / SET_DEATHMATCH_START)")
    ap.add_argument("levels", nargs="*", help="имена уровней без .sbt (по умолчанию все)")
    ap.add_argument("--game", default=DEFAULT_GAME, help="каталог игры")
    ap.add_argument("--json", action="store_true", help="вывести JSON")
    ap.add_argument("--no-geom", action="store_true", help="не читать .sbw")
    ap.add_argument("--chassis", default=DEFAULT_CHASSIS,
                    help="шасси для высоты камеры (по умолчанию %(default)s)")
    ap.add_argument("--eye", type=float,
                    help="высота камеры вручную, перекрывает --chassis")
    args = ap.parse_args(argv)

    files = sorted(glob.glob(os.path.join(args.game, "*.sbt")))
    if not files:
        print("no .sbt in %s" % args.game, file=sys.stderr)
        return 1
    if args.levels:
        want = {x.lower() for x in args.levels}
        files = [p for p in files
                 if os.path.splitext(os.path.basename(p))[0].lower() in want]
        if not files:
            print("levels not found: %s" % " ".join(sorted(want)), file=sys.stderr)
            return 1

    data = []
    for path in files:
        try:
            data.append(level_data(path, args.game, geom=not args.no_geom,
                                   chassis=args.chassis, eye=args.eye))
        except Exception as exc:          # noqa: BLE001 - один битый файл
            data.append({"level": os.path.basename(path), "error": str(exc)})

    if args.json:
        print(json.dumps({"game": args.game, "levels": data}, indent=2,
                         ensure_ascii=False, sort_keys=False))
        return 0

    camp = 0
    dm = 0
    outside = 0
    for d in data:
        if "error" in d:
            print("%s: ERROR %s\n" % (d["level"], d["error"]))
            continue
        print_level(d)
        camp += d["counts"]["campaign"]
        dm += d["counts"]["deathmatch"]
        for r in d["campaign"]:
            if r["gravity"]["inside_world_aabb"] is False:
                outside += 1
    print("итог: уровней %d, точек кампании %d, точек DM %d, вне AABB .sbw %d/%d" % (
        len(data), camp, dm, outside, camp))
    return 0


if __name__ == "__main__":
    sys.exit(main())
