#!/usr/bin/env python3
"""Python-оракул для render-хешей в ``engine/tests/test_main.cpp``.

Каждый хеш в тесте — FNV-1a по RGB-буферу кадра, снятого ``gm/render.py``.
Скрипт повторяет те же кадры (тот же уровень, камера, мип/фильтр) и печатает
числа в виде C++-литералов, чтобы тест оставался паритетом Python ↔ C++, а не
снимком самого C++.

    python3 tools/re/render_goldens.py              # level0 (+ LEVEL10)
    python3 tools/re/render_goldens.py --json

Кадры совпадают с блоками ``test_render`` в ``engine/tests/test_main.cpp``:
world-кадр 320x200, авто-кадр LEVEL10, явная камера (0,0,600)→(0,0,0),
mip 1 + bilinear + alpha test, mip «auto» (LOD bias -0.75), вещи (--things)
400x400 и камера спавна 200x200.

Только чтение установки игры.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import png, render, sbw, scene  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a64(data: bytes, seed: int = FNV_OFFSET) -> int:
    digest = seed
    for byte in data:
        digest ^= byte
        digest = (digest * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return digest


class Capture:
    """``gm.render.render`` returns a PNG; we want the raw RGB it encoded."""

    def __init__(self):
        self.saved = None
        self._orig = png.rgb

    def __enter__(self):
        def fake(width: int, height: int, pixels: bytes) -> bytes:
            self.saved = bytes(pixels)
            return b""

        png.rgb = fake
        render.png.rgb = fake
        return self

    def __exit__(self, *exc):
        png.rgb = self._orig
        return False


def frame(meshes, **kwargs) -> str:
    with Capture() as cap:
        render.render(meshes, **kwargs)
    if cap.saved is None:
        raise RuntimeError("render did not produce an RGB buffer")
    return "0x%016xull" % fnv1a64(cap.saved)


def bounds(meshes):
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for mesh in meshes:
        for v in mesh.vertices:
            for k in range(3):
                lo[k] = min(lo[k], v[k])
                hi[k] = max(hi[k], v[k])
    return lo, hi


def level_case(game: str, name: str, width: int, height: int, out: dict) -> None:
    path = os.path.join(game, name + ".sbw")
    if not os.path.exists(path):
        return
    pool = render.TexturePool(game)
    world = sbw.parse_file(path)
    lms = render.import_lightmaps(os.path.join(game, name + ".sbc"))
    meshes = render.level_meshes(world, lms, transform=True)
    key = name.lower()
    out[key + "_world_%dx%d" % (width, height)] = frame(
        meshes, width=width, height=height, pool=pool)

    lo, hi = bounds(meshes)
    centre = [(lo[k] + hi[k]) / 2.0 for k in range(3)]
    radius = max(1e-6, 0.5 * sum((hi[k] - lo[k]) ** 2 for k in range(3)) ** 0.5)
    out[key + "_centre_eye"] = frame(
        meshes, width=320, height=200, pool=pool,
        pos=(centre[0], centre[1], centre[2] + radius * 0.6),
        look=(centre[0], centre[1], centre[2]))

    out[key + "_explicit_view"] = frame(
        meshes, width=320, height=200, pool=pool, pos=(0.0, 0.0, 600.0),
        look=(0.0, 0.0, 0.0))
    out[key + "_mip1_bilinear"] = frame(
        meshes, width=320, height=200, pool=pool, pos=(0.0, 0.0, 600.0),
        look=(0.0, 0.0, 0.0), mip=1, filter="bilinear", alpha_test=True)
    out[key + "_mip_auto"] = frame(
        meshes, width=320, height=200, pool=pool, pos=(0.0, 0.0, 600.0),
        look=(0.0, 0.0, 0.0), mip="auto", lod_bias=-0.75)
    out[key + "_clip300"] = frame(
        meshes, width=320, height=200, pool=pool, pos=(0.0, 0.0, 600.0),
        look=(0.0, 0.0, 0.0), clip=300.0)

    # --things: every absolute ADD_THING with a model, in world coordinates.
    if name in ("level0", "LEVEL0"):
        sc = scene.load(os.path.join(game, name + ".sbt"))
        with_things = list(meshes)
        for thing in sc.things:
            if not thing.absolute or not thing.model:
                continue
            model_path = os.path.join(game, "SOBS", thing.model + ".sob")
            if not os.path.exists(model_path):
                continue
            from gm import sob
            model = sob.parse(path=model_path)
            with_things.extend(render.model_meshes(render.place_model(model, thing.pos, 1.0)))
        out["things"] = frame(with_things, width=400, height=400, pool=pool,
                              pos=(0.0, 0.0, 600.0), look=(0.0, 0.0, 0.0))

        spawn = next((s for s in sc.spawns
                      if s.kind == 'campaign' and s.index == 0), None)
        if spawn is not None:
            eye, look, _basis = scene.first_person(spawn, eye=10.0)
            out["spawn_camera"] = frame(meshes, width=200, height=200, pool=pool,
                                        pos=eye, look=look)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    out: dict = {}
    level_case(args.game, "level0", 320, 200, out)
    level_case(args.game, "LEVEL10", 320, 200, out)
    if args.json:
        print(json.dumps(out, indent=2, sort_keys=True))
        return 0
    for key in sorted(out):
        print("%-28s %s" % (key, out[key]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
