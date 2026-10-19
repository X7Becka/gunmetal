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
    results.append(check("soa / animations", ["soa", G], ["soa: 104 parsed, 0 failed"]))
    results.append(check("soe / thing types", ["soe", G], ["soe: 1 parsed, 0 failed"]))
    results.append(check("sog / songs", ["sog", f"{G}/MUSIC", "--music", f"{G}/MUSIC/SOU.IDX"],
                         ["sog: 651 songs"]))

    # --- video / lookup tables -------------------------------------------
    results.append(check("sbv / Intro header", ["sbv", f"{G}/Vid/Intro.sbv", "--list-only"]))
    results.append(check("sbv / orient header", ["sbv", f"{G}/Vid/orient.sbv", "--list-only"]))
    results.append(check("sbv / winner header", ["sbv", f"{G}/Vid/winner.sbv", "--list-only"]))
    results.append(check("look / LOOK16", ["look", f"{G}/LOOK16.TAB"], ["234 distinct"]))
    results.append(check("look / LOOK162 vs NEONS.PIN",
                         ["look", f"{G}/LOOK162.TAB", "--neons", f"{G}/NEONS.PIN"],
                         ["20 distinct", "value-set match: True"]))

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
