#!/usr/bin/env python3
"""Parity checks for the opt-in mip / filter / alpha-key sampler in ``gm/render``.

The *default* renderer must stay byte-identical: this script recomputes the
FNV-1a 64 hashes that the C++ renderer hard-codes (``engine/tests/test_main.cpp``)
and compares them with the Python output.  It then exercises the new modes
(explicit mip level, automatic LOD, bilinear filtering, palette-index alpha
test) on real levels/models and on tiny synthetic textures, including the
edge cases (missing mips, 1x1 textures, unknown filter, transparent index
disabled).

Only the standard library is used.  The game is opened read-only; all output
is printed, nothing is written.

Usage::

    python3 tools/parity_mip.py [--game DIR] [--verbose]
"""
from __future__ import annotations

import argparse
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import palette, png, render, sbm, sbw, sob  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# Golden FNV-1a 64 of the raw RGB buffers, as hard-coded in the C++ test.
# The level frames use the world-placed meshes (transform=True), because
# engine/tests/test_main.cpp builds them that way.
MODEL_GOLDENS = (
    ("20mmclp.sob", 0xC376475E890D6879),
    ("MOCLASS.SOB", 0xDFF93879E03CE1AB),
    ("code492.sob", 0xD3FA091674E86A7A),
    ("desertpc.sob", 0x1DE0C4538C49B7C7),
    ("moonfb.sob", 0x4FB421BC5A2FF221),
)
LEVEL0_ORBIT = 0x20E80D077FD4F6BC
LEVEL0_POS = 0x9E54D79F29AC47C6
LEVEL0_CLIP = 0xA4CCDD84A43CA565
LEVEL10_INTERIOR = 0x25E40D77AF2F01BA


class Report:
    def __init__(self, verbose: bool = False):
        self.ok = 0
        self.bad = 0
        self.verbose = verbose

    def check(self, name: str, condition: bool, detail: str = "") -> bool:
        if condition:
            self.ok += 1
            if self.verbose:
                print("PASS  %-52s %s" % (name, detail))
        else:
            self.bad += 1
            print("FAIL  %-52s %s" % (name, detail))
        return condition


def fnv_rgb(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for byte in data:
        h ^= byte
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def rgb_bytes(png_data: bytes) -> bytes:
    return png.decode(png_data).pixels


def render_hash(meshes, width: int, height: int | None = None, **kw) -> int:
    data = render.render(meshes, width, height, **kw)
    return fnv_rgb(rgb_bytes(data))


def coverage_of(png_data: bytes) -> float:
    return render.coverage(png_data)


def golden_checks(rep: Report, G: str, pool: render.TexturePool) -> dict:
    print("Golden FNV-1a 64 (default: mip 0, nearest, no alpha test)")
    for name, expected in MODEL_GOLDENS:
        path = os.path.join(G, "SOBS", name)
        if not os.path.isfile(path):
            rep.check("golden %s" % name, True, "SKIP (file absent)")
            continue
        model = sob.parse(path)
        got = render_hash(render.model_meshes(model), 160, pool=pool)
        rep.check("golden %s" % name, got == expected,
                  "%016x == %016x" % (got, expected))

    level0_path = os.path.join(G, "level0.sbw")
    if not os.path.isfile(level0_path):
        rep.check("golden level0", True, "SKIP (file absent)")
        return {}
    world = sbw.parse_file(level0_path)
    lms = render.import_lightmaps(os.path.join(G, "level0.sbc")) \
        if os.path.isfile(os.path.join(G, "level0.sbc")) else None
    meshes = render.level_meshes(world, lms, transform=True)

    got = render_hash(meshes, 320, 200, pool=pool)
    rep.check("golden level0 orbit", got == LEVEL0_ORBIT,
              "%016x == %016x" % (got, LEVEL0_ORBIT))
    got = render_hash(meshes, 320, 200, pool=pool,
                      pos=(0.0, 0.0, 600.0), look=(0.0, 0.0, 0.0))
    rep.check("golden level0 pos/look", got == LEVEL0_POS,
              "%016x == %016x" % (got, LEVEL0_POS))
    got = render_hash(meshes, 320, 200, pool=pool,
                      pos=(0.0, 0.0, 600.0), look=(0.0, 0.0, 0.0), clip=300.0)
    rep.check("golden level0 pos/clip", got == LEVEL0_CLIP,
              "%016x == %016x" % (got, LEVEL0_CLIP))

    level10_path = os.path.join(G, "LEVEL10.sbw")
    if os.path.isfile(level10_path):
        w10 = sbw.parse_file(level10_path)
        sbc = os.path.join(G, "LEVEL10.sbc")
        lms10 = render.import_lightmaps(sbc) if os.path.isfile(sbc) else None
        meshes10 = render.level_meshes(w10, lms10, transform=True)
        lo = [1e30] * 3
        hi = [-1e30] * 3
        for mesh in meshes10:
            for v in mesh.vertices:
                for k in range(3):
                    lo[k] = min(lo[k], v[k])
                    hi[k] = max(hi[k], v[k])
        center = tuple((lo[k] + hi[k]) / 2.0 for k in range(3))
        radius = max(1e-6, 0.5 * math.sqrt(sum((hi[k] - lo[k]) ** 2
                                               for k in range(3))))
        eye = (center[0], center[1], center[2] + radius * 0.6)
        got = render_hash(meshes10, 320, 200, pool=pool,
                          pos=eye, look=center)
        rep.check("golden LEVEL10 interior", got == LEVEL10_INTERIOR,
                  "%016x == %016x" % (got, LEVEL10_INTERIOR))
    return {"level0": meshes, "world": world}


class _OneTexturePool:
    """Minimal TexturePool stand-in for synthetic .sbm blobs."""

    def __init__(self, tex: render.Texture):
        self.tex = tex

    def get(self, name):
        if name in ("", "none"):
            return None
        return self.tex.rgb(0)

    def get_texture(self, name):
        if name in ("", "none"):
            return None
        return self.tex


def _quad_mesh(texture: str) -> sob.Sob:
    verts = [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0),
             (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)]
    face = sob.SobFace(
        [texture, "none", "none", "none", "none", "none", "none", "none"],
        32768, [0, 1, 2, 3], (0.0, 0.0, 0.0),
        (65536.0, 0.0, 0.0), (0.0, 65536.0, 0.0),
        (0.0, 0.0, 1.0), (0.0, 0.0, 0.0))
    return sob.Sob(verts, [face], "synthetic")


def sampler_checks(rep: Report, G: str, pool: render.TexturePool,
                   level0: list) -> None:
    print("Opt-in modes on real data")
    model_path = os.path.join(G, "SOBS", "Baracuda.sob")
    if os.path.isfile(model_path):
        model = sob.parse(model_path)
        base_data = render.render(model, 160, pool=pool)
        base = fnv_rgb(rgb_bytes(base_data))
        covered = coverage_of(base_data)

        for kw, name in (
            ({"mip": 1}, "explicit mip=1"),
            ({"mip": 5}, "explicit mip=5 (clamped)"),
            ({"mip": "auto"}, "automatic LOD"),
            ({"filter": "bilinear"}, "bilinear filter"),
            ({"mip": "auto", "filter": "bilinear"}, "auto mip + bilinear"),
            ({"alpha_test": True}, "alpha key (index 0)"),
            ({"mip": "auto", "filter": "bilinear", "alpha_test": True},
             "auto + bilinear + alpha"),
        ):
            data = render.render(model, 160, pool=pool, **kw)
            h = fnv_rgb(rgb_bytes(data))
            rep.check(name, h != base, "%016x != %016x" % (h, base))
            rep.check(name + " / coverage", abs(coverage_of(data) - covered) < 0.02,
                      "%.3f vs %.3f" % (coverage_of(data), covered))
            rep.check(name + " / deterministic",
                      render.render(model, 160, pool=pool, **kw) == data, "")

        # Unknown filter falls back to the legacy path.
        h = render.render(model, 160, pool=pool, filter="lanczos")
        rep.check("unknown filter -> nearest", fnv_rgb(rgb_bytes(h)) == base,
                  "%016x" % fnv_rgb(rgb_bytes(h)))

        # alpha_test with transparent_index=None must match the default.
        h = render.render(model, 160, pool=pool, alpha_test=True,
                          transparent_index=None)
        rep.check("alpha_test with no key == default",
                  fnv_rgb(rgb_bytes(h)) == base, "")

        # Negative / huge mip numbers clamp instead of failing.
        h = render.render(model, 160, pool=pool, mip=-3)
        rep.check("mip=-3 clamps to level 0", fnv_rgb(rgb_bytes(h)) == base, "")

    if level0:
        print("Automatic LOD on level0 (world-placed meshes)")
        base_data = render.render(level0, 320, 200, pool=pool)
        base = fnv_rgb(rgb_bytes(base_data))
        covered = coverage_of(base_data)
        auto_data = render.render(level0, 320, 200, pool=pool, mip="auto")
        auto = fnv_rgb(rgb_bytes(auto_data))
        rep.check("level0 auto LOD differs", auto != base,
                  "%016x != %016x" % (auto, base))
        rep.check("level0 auto LOD coverage",
                  abs(coverage_of(auto_data) - covered) < 0.02,
                  "%.3f vs %.3f" % (coverage_of(auto_data), covered))
        bil = render.render(level0, 320, 200, pool=pool, filter="bilinear")
        rep.check("level0 bilinear differs", fnv_rgb(rgb_bytes(bil)) != base, "")
        rep.check("level0 mip=2 differs",
                  render_hash(level0, 320, 200, pool=pool, mip=2) != base, "")


def synthetic_checks(rep: Report, G: str) -> None:
    print("Synthetic textures and edge cases")
    pal = palette.installed(G)

    # 4x4 checkerboard: even texels are palette index 0 (the key colour).
    pix = bytes(0 if (x + y) % 2 == 0 else 200 for y in range(4) for x in range(4))
    tex = render.Texture("KEYED", sbm.parse(sbm.encode(4, 4, pix, levels=3)),
                         pal, "pics")
    pool = _OneTexturePool(tex)
    mesh = _quad_mesh("KEYED")

    opaque = render.render(mesh, 64, pool=pool)
    keyed = render.render(mesh, 64, pool=pool, alpha_test=True)
    rep.check("4x4 keyed alpha changes the frame", opaque != keyed, "")
    rep.check("4x4 keyed alpha removes pixels",
              coverage_of(keyed) < coverage_of(opaque),
              "%.3f < %.3f" % (coverage_of(keyed), coverage_of(opaque)))
    rep.check("4x4 alpha pseudorandom index",
              render.render(mesh, 64, pool=pool, alpha_test=True,
                            transparent_index=None) == opaque, "")

    m2 = render.render(mesh, 64, pool=pool, mip=2)
    rep.check("4x4 missing mip clamps to last",
              render.render(mesh, 64, pool=pool, mip=99) == m2, "")
    rep.check("4x4 auto/bilinear render", len(render.render(
        mesh, 64, pool=pool, mip="auto", filter="bilinear")) > 0, "")

    # A gradient guarantees that box-downsampled mips are all different.
    grad = bytes(10 + (x * 30 + y * 60) % 200 for y in range(4) for x in range(4))
    gtex = render.Texture("GRAD", sbm.parse(sbm.encode(4, 4, grad, levels=3)),
                          pal, "pics")
    gpool = _OneTexturePool(gtex)
    g0 = render.render(mesh, 64, pool=gpool, mip=0)
    g1 = render.render(mesh, 64, pool=gpool, mip=1)
    g2 = render.render(mesh, 64, pool=gpool, mip=2)
    rep.check("4x4 gradient mip levels differ", len({g0, g1, g2}) == 3, "")
    rep.check("4x4 gradient missing mip clamps to last",
              render.render(mesh, 64, pool=gpool, mip=99) == g2, "")

    # 1x1: every mip is the same texel; level selection must not crash.
    one = render.Texture("ONE", sbm.parse(sbm.encode(1, 1, b"\x05")), pal, "pics")
    opool = _OneTexturePool(one)
    o0 = render.render(mesh, 32, pool=opool)
    rep.check("1x1 texture default", len(o0) > 0, "")
    rep.check("1x1 mip=7 == level 0",
              render.render(mesh, 32, pool=opool, mip=7) == o0, "")
    rep.check("1x1 auto+bilinear works",
              render.render(mesh, 32, pool=opool, mip="auto",
                            filter="bilinear") == o0, "")

    # A texture with a single level: mip/auto must fall back to level 0.
    flat = render.Texture("FLAT", sbm.parse(sbm.encode(4, 4, pix, levels=1)),
                          pal, "pics")
    fpool = _OneTexturePool(flat)
    f0 = render.render(mesh, 48, pool=fpool)
    rep.check("single-level mip=5 == level 0",
              render.render(mesh, 48, pool=fpool, mip=5) == f0, "")
    rep.check("single-level auto == level 0",
              render.render(mesh, 48, pool=fpool, mip="auto") == f0, "")

    # Palette indices out of range are mapped to 0 exactly as before.
    bad = render.Texture("BAD", sbm.parse(sbm.encode(2, 2, b"\xff\xff\xff\xff")),
                         pal, "pics")
    bpool = _OneTexturePool(bad)
    b0 = render.render(mesh, 32, pool=bpool)
    rep.check("out-of-range palette index renders", len(b0) > 0, "")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME),
                    help="Gunmetal installation (read-only)")
    ap.add_argument("--verbose", action="store_true", help="print every PASS")
    opts = ap.parse_args(argv)

    if not os.path.isdir(opts.game):
        print("SKIP: game directory not found: %s" % opts.game)
        return 0

    rep = Report(opts.verbose)
    pool = render.TexturePool(opts.game)
    frames = golden_checks(rep, opts.game, pool)
    sampler_checks(rep, opts.game, pool, frames.get("level0", []))
    synthetic_checks(rep, opts.game)

    print("\n%d passed, %d failed" % (rep.ok, rep.bad))
    return 0 if rep.bad == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
