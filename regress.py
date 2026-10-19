#!/usr/bin/env python3
"""Gunmetal toolkit regression.

Runs the ``gm.py`` CLI against an installation and checks the summary lines
(counts and "0 failed").  Every shipped level/asset family is exercised, so a
format module that stops parsing (or a byte-offset regression) is caught here.

    python3 regress.py                       # fast checks
    python3 regress.py --full                # also extract textures/sounds/sprites
    python3 regress.py --game "D:/games/Gunmetal 3dfx" --out /tmp/opencode/regress

Exit code is 0 only when every check passes.  Nothing in the game directory is
written; all output goes to ``--out``.
"""

from __future__ import annotations

import argparse
import glob
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GM = os.path.join(HERE, "gm.py")
DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"


def run(args: list[str]) -> tuple[int, str]:
    p = subprocess.run(
        [sys.executable, GM, *args],
        capture_output=True, text=True, cwd=HERE,
    )
    return p.returncode, (p.stdout + p.stderr)


def check(name: str, args: list[str], expect: list[str] | None = None) -> bool:
    rc, out = run(args)
    tail = out.strip().splitlines()[-1] if out.strip() else "(no output)"
    ok = rc == 0
    for e in expect or []:
        if e not in out:
            ok = False
    status = "ok  " if ok else "FAIL"
    print(f"[{status}] {name}")
    if not ok:
        print(f"        cmd: gm.py {' '.join(args)}")
        print(f"        last: {tail}")
        for e in expect or []:
            if e not in out:
                print(f"        missing: {e!r}")
    return ok


def report(name: str, ok: bool, detail: str = "") -> bool:
    print(f"[{'ok  ' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if ok and detail else ""))
    if not ok and detail:
        print(f"        {detail}")
    return ok


def render_checks(G: str, results: list[bool], out: str, full: bool = False) -> None:
    """Model/level rendering: deterministic PNGs with a sane coverage."""
    sys.path.insert(0, HERE)
    from gm import png, render, sbw, sob

    pool = render.TexturePool(G)
    tex = pool.get("SHELLS1")
    results.append(report("render / texture pool",
                          tex is not None and tex[0] > 0 and tex[1] > 0,
                          "SHELLS1 resolved from PICS"))

    m = sob.parse(os.path.join(G, "SOBS", "20mmclp.sob"))
    a = render.render(m, 96, pool=pool, az=35, el=22)
    b = render.render(m, 96, pool=pool, az=35, el=22)
    img = png.decode(a)
    cov = render.coverage(a)
    ok = (a == b and img.width == 96 and img.height == 96 and img.channels == 3
          and 0.15 < cov < 0.9)
    results.append(report("render / model (deterministic)", ok,
                          f"96x96 RGB, coverage {cov:.1%}"))

    w = sbw.parse_file(os.path.join(G, "level0.sbw"))
    meshes = render.level_meshes(w)
    lv = render.render(meshes, 96, pool=pool, az=35, el=40)
    cov = render.coverage(lv)
    n_faces = sum(len(x.faces) for x in meshes)
    results.append(report("render / level (level0.sbw)", 0.05 < cov < 0.95,
                          f"{n_faces} faces, coverage {cov:.1%}"))

    inside = render.render(meshes, 96, pool=pool, pos=(0.0, 0.0, 20.0),
                           look=(60.0, 60.0, 20.0), clip=200.0)
    results.append(report("render / level interior (pos/look/clip)",
                          render.coverage(inside) > 0.5, "camera inside level0"))

    r = sob.parse(os.path.join(G, "SOBS", "rays10.sob"))
    on = render.render(r, 64, pool=pool, az=35, el=22)
    off = render.render(r, 64, pool=pool, az=35, el=22, materials=False)
    n_blend = sum(1 for f in r.faces if render.face_blend(f))
    results.append(report("render / blended materials", on != off,
                          f"{n_blend} additive faces change the image"))

    # lightmaps: <level>.sbc sampling (0x43ca5a..0x43cc9c) + projected texels
    lms = render.import_lightmaps(os.path.join(G, "level0.sbc"))
    lmsh = render.level_meshes(w, lms)
    lit = render.render(lmsh, 96, pool=pool, az=35, el=40)
    unlit = render.render(lmsh, 96, pool=pool, az=35, el=40, lightmap=False)
    inside = total = 0
    for m in lmsh:
        for f in m.faces:
            if f.lightmap < 0:
                continue
            page = m.lightmaps.page(f)
            for i in f.indices:
                u, v = m.lightmaps.uv(f, m.vertices[i], mask=False)
                total += 1
                inside += 0 <= u < page.width and 0 <= v < page.height
    results.append(report("render / lightmap projection",
                          total and inside / total > 0.99,
                          f"{inside}/{total} vertices inside their page"))
    results.append(report("render / lightmaps change shading", lit != unlit,
                          f"{len(lms.records)} records, {(lit, unlit) and 'ok'}"))

    # sector/object frustum culling must be conservative: same image, less work
    eye, tgt = (-416.5, -1140.0, -602.0), (-416.5, -1140.0, -342.0)
    ms2 = render.level_meshes(sbw.parse_file(os.path.join(G, "dm1.sbw")),
                              render.import_lightmaps(os.path.join(G, "dm1.sbc")))
    full = render.render(ms2, 200, pool=pool, pos=eye, look=tgt, clip=1200, sectors=False)
    culled = render.render(ms2, 200, pool=pool, pos=eye, look=tgt, clip=1200, sectors=True)
    n_all = sum(len(m.faces) for m in ms2)
    results.append(report("render / sector culling is exact", full == culled,
                          f"dm1 {n_all} faces, identical image"))

    # near-plane clipping: a quad straddling the eye must still fill the screen
    class _Face:
        indices = [0, 1, 2, 3, 4]
        texture = "none"
        normal = (0.0, 0.0, -1.0)
        material = 0
        brightness = 0
        material_flags: tuple = ()

    class _Straddle:
        # one vertex sits behind the near plane: the renderer must clip, not drop
        vertices = [(0.0, 0.0, -0.02), (-300.0, -300.0, -100.0), (300.0, -300.0, -100.0),
                    (300.0, 300.0, -100.0), (-300.0, 300.0, -100.0)]
        faces = [_Face()]

        def uv(self, face, v):
            return (v[0] / 600.0, v[1] / 600.0)

    quad = render.render([_Straddle()], 96, pool=pool, pos=(0.0, 0.0, 0.0),
                         look=(0.0, 0.0, -1.0), solid=True)
    results.append(report("render / near-plane clipping", render.coverage(quad) > 0.9,
                          "polygon straddling the eye keeps its visible part"))

    if full:
        bad = 0
        files = sorted(glob.glob(os.path.join(G, "SOBS", "*.sob")))
        for p in files:
            try:
                src = sob.parse(p)
                data = render.render(src, 48, pool=pool, az=35, el=22)
                if png.decode(data).width != 48:
                    bad += 1
            except Exception:  # noqa: BLE001 - count and continue
                bad += 1
        results.append(report("render / all models", bad == 0,
                              f"{len(files)} models rendered"))


def sbv_checks(G: str, results: list[bool], out: str) -> None:
    """Video container writer: rebuild each clip from its own frame blocks.

    The payload codec is intra-coded, so a block needs no context: the frame
    table can be regenerated from the block sizes and the file comes back byte
    for byte.  Both the in-process writer and the CLI raw-block path are pinned.
    """
    sys.path.insert(0, HERE)
    from gm import sbv

    clips = sorted(glob.glob(os.path.join(G, "Vid", "*.sbv")))
    bad = []
    for p in clips:
        clip, blob = sbv.parse_file(p)
        if clip.to_bytes(blob) != blob:
            bad.append(os.path.basename(p))
    results.append(report("sbv / rebuild round-trip", not bad,
                          f"{len(clips)} clips byte-identical"))

    raw = os.path.join(out, "sbv_raw")
    built = os.path.join(out, "sbv_built.sbv")
    src = os.path.join(G, "Vid", "Intro.sbv")
    run(["sbv", src, raw])
    run(["sbv", raw, built])
    ok = os.path.exists(built) and open(built, "rb").read() == open(src, "rb").read()
    results.append(report("sbv / raw blocks -> clip", ok,
                          "Intro.sbv encoded back through frames.txt"))


def scene_checks(G: str, results: list[bool], out: str) -> None:
    """Scripted scene extraction (gm.scene) and its viewer overlay.

    The shipped levels carry no placed-object list at all, so the scene is
    decoded out of the compiled ``SCRIPT`` code; these checks pin the operand
    layouts down to the engine handlers (see the gm/scene.py docstring) and make
    sure the viewer overlay stays wired to them.
    """
    sys.path.insert(0, HERE)
    from gm import render, sbt, sbtvm, sbw, scene

    def _script(path):
        return [r.payload for r in sbt.parse_file(path).records if r.type == 11][0]

    def _walk_all(insns):
        for i in insns:
            yield i
            if isinstance(i.body, list):
                yield from _walk_all(i.body)

    sc = scene.load(os.path.join(G, "dm1.sbt"))
    cnt = sc.counts()
    got = (cnt["lights"], cnt["locations"], cnt["items"], cnt["sounds"])
    results.append(report("scene / dm1 extraction", got == (60, 8, 1, 21),
                          "dm1: %d lights, %d locations, %d items, %d sounds" % got))
    # ADD_SKY_LIGHT_SOURCE is stored as (x, y, z, param, r, g, b) by 0x47e130
    sky = [l for l in sc.lights if l.kind == "sky"]
    results.append(report(
        "scene / proven sky-light operands",
        len(sky) == 2 and sky[0].pos == (-1040, -2568, 3526)
        and sky[0].rgb == (255, 255, 255) and sky[0].param == 112,
        "sky lights: " + ", ".join(f"{l.pos} rgb={l.rgb}" for l in sky)))

    tot = {"lights": 0, "locations": 0, "items": 0, "sounds": 0, "sprites": 0}
    files = sorted(glob.glob(os.path.join(G, "*.sbt")))
    bad = 0
    for f in files:
        try:
            c = scene.load(f).counts()
        except Exception:  # noqa: BLE001 - count and continue
            bad += 1
            continue
        for k in tot:
            tot[k] += c[k]
    results.append(report(
        "scene / all levels decode",
        bad == 0 and tot["lights"] == 4470 and tot["locations"] == 461
        and tot["sounds"] == 1173 and tot["sprites"] == 555,
        "%d levels: %d lights, %d locations, %d items, %d sounds, %d sprites, "
        "%d failed" % (len(files), tot["lights"], tot["locations"], tot["items"],
                       tot["sounds"], tot["sprites"], bad)))

    # Coordinate frame: scripted coordinates are engine world, i.e. 1.875x the
    # .sbw file frame (the object header basis).  Unlike the biased "inside the
    # AABB" test, this spread ratio is scale-covariant: sprite positions are
    # props that must sit inside their level, so the ratio has to come out as
    # the basis itself.
    import statistics

    ratios = []
    for f in files:
        try:
            wb = sbw.parse_file(os.path.splitext(f)[0] + ".sbw")
            spr = scene.load(f).sprites
        except Exception:  # noqa: BLE001 - skip levels without a sibling .sbw
            continue
        if len(spr) < 4:
            continue
        lo, hi = [1e30] * 3, [-1e30] * 3
        for s in wb.sectors:
            for o in s.objects:
                vs = o.vertices
                if not vs:
                    continue
                ext = max(max(v[k] for v in vs) - min(v[k] for v in vs)
                          for k in range(3))
                if ext > 4000:  # sky box
                    continue
                for v in vs:
                    for k in range(3):
                        lo[k] = min(lo[k], v[k])
                        hi[k] = max(hi[k], v[k])
        gspan = [hi[k] - lo[k] for k in range(3)]
        pspan = [max(p.pos[k] for p in spr) - min(p.pos[k] for p in spr)
                 for k in range(3)]
        r = [pspan[k] / gspan[k] for k in range(3) if gspan[k] > 1]
        if r:
            ratios.append(statistics.median(r))
    med = statistics.median(ratios) if ratios else 0.0
    results.append(report("scene / world frame is the 1.875 basis",
                          1.2 <= med <= 2.4 and len(ratios) >= 15,
                          "sprite/geometry spread ratio %.2f over %d levels"
                          % (med, len(ratios))))

    # the SCRIPT handle columns are the level's entity table: (flags, id,
    # record offset).  Its ids are what the ADD_* commands address entities by.
    ents = scene.entities(_script(os.path.join(G, "dm1.sbt")))
    ids = {e[0] & 0xFFFFFFFF for e in ents}
    ins, _ = sbtvm.decode(_script(os.path.join(G, "dm1.sbt")).code)
    switched = {i.args[1] & 0xFFFFFFFF for i in _walk_all(ins)
                if getattr(i, "op", None) == 183 and len(i.args) > 1}
    flags_ok = {e[1] for e in ents} <= {0, 511, 1023}
    results.append(report(
        "scene / entity table",
        len(ents) == 363 and flags_ok and len(switched & ids) >= 25,
        "dm1: %d entities, flags %s, %d/%d unique ADD_SWITCH ids in the table"
        % (len(ents), sorted({e[1] for e in ents}), len(switched & ids),
           len(switched))))

    # column 2 is the offset of the entity's *sub-script* in the compiled code
    # (GENERATE runs it through 0x457390); the *steps* between consecutive
    # entities repeat level to level (the levels share the editor templates) --
    # they are the block sizes (6, 11, 34, 122, 994, 3 bytes).
    steps_dm1 = [ents[i + 1][2] - ents[i][2] for i in range(1, 7)]
    ents13 = scene.entities(_script(os.path.join(G, "level13.sbt")))
    steps_13 = [ents13[i + 1][2] - ents13[i][2] for i in range(1, 7)]
    results.append(report("scene / entity sub-script offsets",
                          steps_dm1 == steps_13 and steps_dm1 == [6, 11, 34, 122, 994, 3],
                          "steps 2..7 %s (identical in level13)" % steps_dm1))

    # the overlay and the light shading must reach the renderer
    pool = render.TexturePool(G)
    w = sbw.parse_file(os.path.join(G, "dm1.sbw"))
    lms = render.import_lightmaps(os.path.join(G, "dm1.sbc"))
    ms = render.level_meshes(w, lms)
    base = render.render(ms, 96, pool=pool, el=35)
    over = render.render(ms, 96, pool=pool, el=35, markers=sc.markers())
    lit = render.render(ms, 96, pool=pool, el=35, lights=sc.render_lights())
    results.append(report("scene / markers reach the image", base != over,
                          "%d scene markers drawn" % len(sc.markers())))
    results.append(report("scene / scene lights shade the level", base != lit,
                          "%d light sources feed _scene_shade" % len(sc.render_lights())))


def write_path_checks(G: str, results: list[bool], out: str) -> None:
    """In-process round-trip checks for the writers (sbm blobs, png->sbm)."""
    sys.path.insert(0, HERE)
    from gm import bigfile, sbm, png, palette

    pal = palette.installed(G)
    bad = n = 0
    for rel in ("PICS/SBM.IDX", "sprites/SBM.IDX"):
        bf = bigfile.BigFile(os.path.join(G, rel))
        dat = open(bf.dat_path, "rb").read()
        for e in bf.entries:
            blob = e.data(dat)
            s = sbm.parse(blob)
            n += 1
            if s.to_bytes() != blob:
                bad += 1
    results.append(report("sbm / blob round-trip", bad == 0, f"{n} blobs byte-identical"))

    bf = bigfile.BigFile(os.path.join(G, "sprites/SBM.IDX"))
    dat = open(bf.dat_path, "rb").read()
    bad = 0
    sample = bf.entries[:48]
    for e in sample:
        blob = e.data(dat)
        s = sbm.parse(blob)
        img = png.decode(s.to_png(pal))          # indexed PNG preserves indices
        re = sbm.encode(s.width, s.height, img.pixels, levels=s.levels, flags=s.flags)
        if re != blob:
            bad += 1
    results.append(report("sbm / png->sbm round-trip", bad == 0,
                          f"{len(sample)} sprites byte-identical"))

    # sbw: re-serialise every level and compare byte-for-byte
    rb = os.path.join(out, "sbw_rebuild")
    run(["sbw", *sorted(glob.glob(os.path.join(G, "*.sbw"))), "--rebuild", rb])
    same = True
    for p in sorted(glob.glob(os.path.join(G, "*.sbw"))):
        rp = os.path.join(rb, os.path.basename(p))
        if not os.path.exists(rp) or open(rp, "rb").read() != open(p, "rb").read():
            same = False
            break
    results.append(report("sbw / rebuild round-trip", same, "36 files byte-identical"))

    # sbt: re-serialise + structured re-encode of the modelled records
    from gm import sbt as sbtmod
    sbt_paths = sorted(glob.glob(os.path.join(G, "*.sbt")))
    rb = os.path.join(out, "sbt_rebuild")
    run(["sbt", G, "--rebuild", rb])
    same = True
    for p in sbt_paths:
        rp = os.path.join(rb, os.path.basename(p))
        if not os.path.exists(rp) or open(rp, "rb").read() != open(p, "rb").read():
            same = False
            break
    results.append(report("sbt / rebuild round-trip", same, f"{len(sbt_paths)} files byte-identical"))

    enc_ok = enc_tot = 0
    for p in sbt_paths:
        s = sbtmod.parse(open(p, "rb").read())
        for rec in s.records:
            if rec.type in (4, 5, 15, 16, 17, 20):
                enc_tot += 1
                if sbtmod.encode_payload(rec.type, rec.payload) == rec.raw:
                    enc_ok += 1
    results.append(report("sbt / structured re-encode", enc_ok == enc_tot and enc_tot > 0,
                          f"{enc_ok}/{enc_tot} records exact"))

    # sbt: SCRIPT (type 11) bytecode must decode to the exact end of every block
    from gm import sbtvm
    dec_ok = dec_tot = 0
    for p in sbt_paths:
        s = sbtmod.parse(open(p, "rb").read())
        for rec in s.records_of(11):
            dec_tot += 1
            try:
                _ins, end = sbtvm.decode(rec.payload.code)
            except ValueError:
                end = -1
            if end == len(rec.payload.code):
                dec_ok += 1
    results.append(report("sbt / SCRIPT VM decode", dec_ok == dec_tot and dec_tot > 0,
                          f"{dec_ok}/{dec_tot} code blocks decode to exact end"))

    # text/binary descriptor writers: rebuild every shipped file
    from gm import sbc as sbcmod, soa as soamod, sog as sogmod, soe as soemod

    def all_ext(root: str, ext: str) -> list[str]:
        found = []
        for r, _d, fs in os.walk(root):
            found += [os.path.join(r, f) for f in fs if f.lower().endswith(ext)]
        return sorted(found)

    cases = [
        ("sbc", ".sbc", sbcmod.parse),
        ("soa", ".soa", soamod.parse),
        ("sog", ".sog", sogmod.parse),
        ("soe", ".soe", lambda d: soemod.parse_text(d.decode("latin1"))),
    ]
    for label, ext, parsefn in cases:
        bad = n = 0
        for p in all_ext(G, ext):
            raw = open(p, "rb").read()
            n += 1
            if parsefn(raw).to_bytes() != raw:
                bad += 1
        results.append(report(f"{label} / rebuild round-trip", bad == 0,
                              f"{n} files byte-identical"))


def sml_checks(G: str, results: list[bool]) -> None:
    """The ``.sme`` cipher: decode matches the ``.sml`` twins and round-trips."""
    sys.path.insert(0, HERE)
    from gm import sml

    root = os.path.join(G, "SML")
    pairs = same = diff = enc_ok = enc_tot = 0
    for base, _dirs, files in os.walk(root):
        for f in sorted(files):
            if os.path.splitext(f)[1].lower() != ".sme":
                continue
            p = os.path.join(base, f)
            blob = open(p, "rb").read()
            enc_tot += 1
            if sml.encode(sml.decode(blob)) == blob:
                enc_ok += 1
            twin = os.path.join(base, os.path.splitext(f)[0] + ".sml")
            if os.path.exists(twin):
                pairs += 1
                if sml.decode(blob) == open(twin, "rb").read():
                    same += 1
                else:
                    diff += 1
    results.append(report("sml / .sme cipher round-trip",
                          enc_tot >= 150 and enc_ok == enc_tot,
                          "%d/%d .sme re-encode byte-identically" % (enc_ok, enc_tot)))
    results.append(report("sml / decode matches the .sml twins",
                          same == 131 and diff == 16,
                          "%d pairs: %d decode byte-identically, %d are revisions"
                          % (pairs, same, diff)))

    # tag semantics: the engine dispatches only on TAG_ROLES (0x497d30 through
    # the class table 0x498448 / jump table 0x4983f8); <k> is the callsign.
    # decode_auto also validates the shift through the tag letters.
    seen: set = set()
    bad = repaired = 0
    for base, _dirs, files in os.walk(root):
        for f in files:
            if os.path.splitext(f)[1].lower() not in (".sml", ".sme"):
                continue
            p = os.path.join(base, f)
            blob = open(p, "rb").read()
            if f.lower().endswith(".sme"):
                text, fixed = sml.decode_auto(blob)
                repaired += fixed
            else:
                text = blob.decode("latin1")
            letters = sml.tag_letters(text)
            seen |= letters
            if not letters <= sml.SHIPPED_TAGS:
                bad += 1
    active = {c for c in sml.TAG_ROLES if c != "/"}
    results.append(report("sml / tag vocabulary",
                          bad == 0 and repaired == 1 and seen == sml.SHIPPED_TAGS,
                          "%d tag letters (%s; the engine handles %s, d/D are "
                          "ignored); %d repaired"
                          % (len(seen), "".join(sorted(seen)),
                             "".join(sorted(active)), repaired)))
    results.append(report("sml / placeholder semantics",
                          sml.plain("Morning, <k>.") == "Morning, Soldier."
                          and sml.plain("<r><c>A</c><c>B</c>") == "A  B"
                          and sml.plain("<D>x") == "x",
                          "<k> -> Soldier (ds:0x4bc2b8), <c> separates columns, "
                          "<d>/<D> ignored"))


def sbw_field_checks(G: str, results: list[bool]) -> None:
    """Scan all levels for the invariants behind the residual ``.sbw`` fields."""
    sys.path.insert(0, HERE)
    import math
    import struct

    from gm import sbw

    tot = u_ok = v_ok = 0
    kinds: set[int] = set()
    flags: set[int] = set()
    f32_bad = 0
    for path in sorted(glob.glob(os.path.join(G, "*.sbw"))):
        w = sbw.parse_file(path)
        for s in w.sectors:
            kinds.add(s.kind)
            for o in s.objects:
                for p in o.polys:
                    tot += 1
                    if p.u_scale == ((_s32(p.uv_u_fp) >> 7) & 0xFFFF):
                        u_ok += 1
                    if p.v_scale == ((_s32(p.uv_v_fp) >> 7) & 0xFFFF):
                        v_ok += 1
                    flags.add(p.flags)
                    for lo, hi in ((p.scroll_u, p.anim0), (p.scroll_v, p.anim1)):
                        f = struct.unpack("<f", struct.pack("<I", (hi << 16) | lo))[0]
                        if not math.isfinite(f) or f == 0.0:
                            f32_bad += 1
    results.append(report(
        "sbw / u_scale is derived (uv_fp >> 7)",
        u_ok >= tot - 12 and v_ok >= tot - 10,
        "%d polys: u %d, v %d match (text loader 0x425db4/0x425ddc)"
        % (tot, u_ok, v_ok)))
    results.append(report("sbw / poly flags", flags == {0x8000, 0x8002, 0x8004, 0x8006},
                          "flag set %s (0x8000 = UV/frame valid)"
                          % sorted(hex(f) for f in flags)))
    results.append(report("sbw / sector kind = 6 x type",
                          kinds == {0, 6, 12, 18, 24},
                          "kinds %s (SET_SECTOR_TYPE writes type*6)"
                          % sorted(kinds)))
    results.append(report("sbw / anim pairs are f32", f32_bad == 0,
                          "%d polys, %d (0x5c,0x5e)/(0x60,0x62) pairs are finite f32"
                          % (tot, 2 * tot)))


def _s32(v: int) -> int:
    return v - 0x100000000 if v >= 0x80000000 else v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME, help="Gunmetal installation directory")
    ap.add_argument("--out", help="output directory for extraction checks")
    ap.add_argument("--full", action="store_true",
                    help="also extract textures/sounds/sprites/frontend (slow, writes many files)")
    opts = ap.parse_args()

    G = opts.game
    out = opts.out or os.path.join(tempfile.gettempdir(), "opencode", "regress")
    os.makedirs(out, exist_ok=True)
    results: list[bool] = []

    def sbw_files() -> list[str]:
        return sorted(glob.glob(os.path.join(G, "*.sbw")))

    # --- archives ---------------------------------------------------------
    results.append(check("archives / PICS",     ["list", f"{G}/PICS/SBM.IDX"],     ["1648 entries"]))
    results.append(check("archives / sprites",  ["list", f"{G}/sprites/SBM.IDX"],  ["179 entries"]))
    results.append(check("archives / sounds",   ["list", f"{G}/sounds/SOU.IDX"],   ["999 entries"]))
    results.append(check("archives / frontend", ["list", f"{G}/FRONTEND/BMP.IDX"], ["143 entries"]))
    results.append(check("archives / music",    ["list", f"{G}/MUSIC/SOU.IDX"],    ["3092 entries"]))

    # --- BIGFILE write path: extract -> pack must reproduce the archive ----
    raw = os.path.join(out, "raw_spr")
    rep = os.path.join(out, "repack.IDX")
    run(["extract", f"{G}/sprites/SBM.IDX", raw])
    run(["pack", rep, raw, "--ext", ".bin"])
    idx_same = (os.path.exists(rep)
                and open(rep, "rb").read() == open(f"{G}/sprites/SBM.IDX", "rb").read())
    dat_same = (os.path.exists(os.path.join(out, "repack.DAT"))
                and open(os.path.join(out, "repack.DAT"), "rb").read()
                == open(f"{G}/sprites/SBM.DAT", "rb").read())
    results.append(report("repack / sprites round-trip", idx_same and dat_same,
                          "IDX+DAT byte-identical"))
    mod = os.path.join(out, "mod.IDX")
    blob = os.path.join(out, "regress_blob.bin")
    with open(blob, "wb") as f:
        f.write(b"REGRESS!")
    rc, _ = run(["set", f"{G}/sprites/SBM.IDX", "REGTEST", blob, mod])
    ok = rc == 0
    if ok:
        rc2, out2 = run(["list", mod])
        ok = rc2 == 0 and "REGTEST" in out2
    results.append(report("repack / set adds an entry", ok, "REGTEST present"))

    # --- in-process writer round-trips -----------------------------------
    write_path_checks(G, results, out)

    # --- level formats ----------------------------------------------------
    results.append(check("sbw / all levels", ["sbw", *sbw_files()], ["sbw: 36 parsed, 0 failed"]))
    results.append(check("sbt / all levels + resolve", ["sbt", G, "--resolve"],
                         ["sbt: 36 parsed, 0 failed"]))
    results.append(check("sbc / all levels",
                         ["sbc", *sorted(glob.glob(os.path.join(G, "*.sbc"))), out],
                         ["sbc: 36 files, 0 failed"]))

    # --- models / animations / audio -------------------------------------
    results.append(check("sob / models", ["sob", f"{G}/SOBS", out], ["sob: 672 converted, 0 failed"]))
    render_checks(G, results, out, opts.full)
    scene_checks(G, results, out)
    results.append(check("soa / animations", ["soa", G], ["soa: 104 parsed, 0 failed"]))
    results.append(check("soe / thing types", ["soe", G], ["soe: 1 parsed, 0 failed"]))
    results.append(check("sog / songs", ["sog", f"{G}/MUSIC", "--music", f"{G}/MUSIC/SOU.IDX"],
                         ["sog: 651 songs"]))

    # --- video / lookup tables -------------------------------------------
    results.append(check("sbv / Intro header", ["sbv", f"{G}/Vid/Intro.sbv", "--list-only"]))
    results.append(check("sbv / orient header", ["sbv", f"{G}/Vid/orient.sbv", "--list-only"]))
    results.append(check("sbv / winner header", ["sbv", f"{G}/Vid/winner.sbv", "--list-only"]))
    sbv_checks(G, results, out)
    results.append(check("look / LOOK16", ["look", f"{G}/LOOK16.TAB"], ["234 distinct"]))
    results.append(check("look / LOOK162 vs NEONS.PIN",
                         ["look", f"{G}/LOOK162.TAB", "--neons", f"{G}/NEONS.PIN"],
                         ["20 distinct", "value-set match: True"]))

    # --- residual .sbw fields (fast: parses all 36 levels in ~2 s) --------
    sbw_field_checks(G, results)

    # --- .sml/.sme markup -------------------------------------------------
    sml_checks(G, results)
    results.append(check("sml / extract markup + text",
                         ["sml", f"{G}/SML", os.path.join(out, "sml")],
                         ["sml: 300 files (151 decoded from .sme, 1 repaired)"]))

    # --- campaign / deathmatch transition map (tools/re/campaign.py) ------
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "campaign.py"),
                        "--game", G], capture_output=True, text=True, cwd=HERE)
    rows = [l for l in p.stdout.splitlines()[1:] if l[:1].isalpha()]
    ok = (p.returncode == 0 and len(rows) == 36
          and "DM5 - TEMPLE" in p.stdout and "secrt1" in p.stdout)
    results.append(report("campaign / transition map", ok,
                          f"{len(rows)} levels, secrt1+temple present"))

    # --- chassis -> craft model (tools/re/chassis_models.py) --------------
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                     "chassis_models.py"),
                        "--game", G], capture_output=True, text=True, cwd=HERE)
    last = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""
    ok = p.returncode == 0 and "8 names in SOB_TABLE: 36/36" in last \
        and "CHEVY: 0/36" in last
    results.append(report("chassis / craft model resolution", ok, last or "no output"))

    # --- thing type -> model (tools/re/thing_models.py) -------------------
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                     "thing_models.py"),
                        "--game", G], capture_output=True, text=True, cwd=HERE)
    last = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""
    ok = p.returncode == 0 and "valid SOB index 2896/2896" in last
    results.append(report("things / type -> model (+0x1a4)", ok, last or "no output"))

    # --- entity table -> sub-script -> thing type (tools/re/entities.py) -----
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                     "entities.py"),
                        "--game", G], capture_output=True, text=True, cwd=HERE)
    tail = [l.strip() for l in p.stdout.strip().splitlines()[-3:]]
    ok = (p.returncode == 0 and len(tail) == 3
          and "blocks ending on the next row 15025/15061" in tail[0]
          and "ADD_THING type operands: 4005 valid, 3 null (0xffffffff), 0 invalid" in tail[1]
          and "GENERATE ids in the entity table: 7923/8957" in tail[2])
    results.append(report("entities / id -> sub-script -> thing type", ok,
                          "; ".join(tail) or "no output"))

    # --- things built by the entity sub-scripts (gm/scene.py) --------------
    from gm import scene as scenemod
    n_things = n_abs = n_models = 0
    models: set = set()
    for p in sorted(glob.glob(os.path.join(G, "*.sbt"))):
        sc = scenemod.load(p)
        for t in sc.things:
            n_things += 1
            if t.absolute:
                n_abs += 1
            if t.model:
                models.add(t.model)
                n_models += 1
    results.append(report("scene / things (ADD_THING* in sub-scripts)",
                          n_things == 4008 and n_abs == 2844
                          and n_models == 4005 and len(models) == 138,
                          "%d things (%d placed, %d models, %d with a model)"
                          % (n_things, n_abs, len(models), n_models)))

    # --- .sbw object header transform (gm/sbw.py, gm/render.py) -----------
    from gm import render as rendermod
    from gm import sbw as sbwmod
    lvl = sbwmod.parse_file(os.path.join(G, "dm1.sbw"))
    ok_tf = 0
    offs = []
    for sec in lvl.sectors:
        for o in sec.objects:
            if not o.vertices:
                continue
            wv = o.world_vertices()
            B = o.points
            ok_tf += 1 if all(
                abs(wv[i][k] - (B[0][k] + B[1][k] * o.vertices[i][0] +
                                B[2][k] * o.vertices[i][1] +
                                B[3][k] * o.vertices[i][2])) < 1e-3
                for i in range(min(len(wv), 20)) for k in range(3)) else 0
            offs.append(max(abs(c) for c in o.points[0]))
            if len(offs) >= 50:
                break
        if len(offs) >= 50:
            break
    # the transform must not change the texture mapping
    obj = lvl.sectors[0].objects[0]
    lm = rendermod.LevelMesh(obj, [t.name for t in lvl.tex], None, 0)
    pm = rendermod.PlacedMesh(obj, [t.name for t in lvl.tex], None, 0)
    uv_ok = True
    if lm.faces and pm.faces:
        f = lm.faces[0]
        v = f.indices[0]
        uv_a = lm.uv(f, lm.vertices[v])
        uv_b = pm.uv(pm.faces[0], pm.vertices[v])
        uv_ok = (abs(uv_a[0] - uv_b[0]) < 0.5 and abs(uv_a[1] - uv_b[1]) < 0.5)
    results.append(report(
        "sbw / object header transform",
        ok_tf == len(offs) and uv_ok and max(offs) < 16,
        "%d objects: world = origin + basis*local, max |origin| %.1f, UV kept: %s"
        % (len(offs), max(offs), uv_ok)))

    # --- script positions vs the level mesh (gm/scene.py) -----------------
    from gm import scene as scenemod
    sc_dm1 = scenemod.load(os.path.join(G, "dm1.sbt"))
    core = []
    for sec in lvl.sectors:
        for o in sec.objects:
            if not o.vertices:
                continue
            vs = o.vertices
            if max(max(v[i] for v in vs) - min(v[i] for v in vs)
                   for i in range(3)) <= 4000:
                core.extend(vs)
    CELL = 150.0
    grid = {}
    for v in core:
        grid.setdefault((int(v[0] // CELL), int(v[1] // CELL),
                         int(v[2] // CELL)), []).append(v)

    def _nearest(p):
        k = (int(p[0] // CELL), int(p[1] // CELL), int(p[2] // CELL))
        best = 1e18
        for dx in range(-4, 5):
            for dy in range(-4, 5):
                for dz in range(-4, 5):
                    for v in grid.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                        d = ((v[0] - p[0]) ** 2 + (v[1] - p[1]) ** 2
                             + (v[2] - p[2]) ** 2)
                        if d < best:
                            best = d
        return best ** 0.5

    sc_scale = scenemod.SCALE
    sw = sorted(_nearest(tuple(c * sc_scale for c in x.pos))
                for x in sc_dm1.switches)
    dr = sorted(min(math.dist(tuple(c * sc_scale for c in d.corner), v)
                    for v in core) for d in sc_dm1.doors)
    med_sw = sw[len(sw) // 2]
    med_dr = dr[len(dr) // 2]
    results.append(report(
        "scene / switch positions land on the mesh",
        med_sw <= 20 and med_dr > 300 and len(sw) == 129 and len(dr) == 61,
        "dm1: switches median %.1f units from a vertex, door corners %.1f "
        "(doors are not in the world frame)" % (med_sw, med_dr)))

    # --- doors / switches / lifts (tools/re/doors.py) ---------------------
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                     "doors.py"),
                        "--game", G], capture_output=True, text=True, cwd=HERE)
    tail = [l.strip() for l in p.stdout.strip().splitlines()[-2:]]
    ok = (p.returncode == 0 and len(tail) == 2
          and "1779 doors (1534 sliding, 200 linked), 3964 switches, 45 lifts" in tail[0]
          and "switch ids in the level's entity table: 3281/3964" in tail[1])
    results.append(report("scene / doors, switches, lifts", ok,
                          "; ".join(tail) or "no output"))

    # --- optional heavy extraction ---------------------------------------
    if opts.full:
        results.append(check("textures / PICS -> PNG",
                             ["textures", G, os.path.join(out, "tex")], ["0 failed"]))
        results.append(check("sprites / sprites -> PNG",
                             ["sprites", G, os.path.join(out, "spr")], ["0 failed"]))
        results.append(check("sounds / sounds -> WAV",
                             ["sounds", G, os.path.join(out, "snd")], ["0 failed"]))
        results.append(check("frontend / BMP.DAT",
                             ["frontend", G, os.path.join(out, "fe")], ["143 files"]))

    passed = sum(results)
    print(f"\nregress: {passed}/{len(results)} checks passed"
          + ("" if opts.full else "  (use --full for extraction checks)"))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
