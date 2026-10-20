#!/usr/bin/env python3
"""Gunmetal toolkit regression.

Runs the ``gm.py`` CLI against an installation and checks the summary lines
(counts and "0 failed").  Every shipped level/asset family is exercised, so a
format module that stops parsing (or a byte-offset regression) is caught here.

    python3 regress.py                       # all fast checks
    python3 regress.py --full                # all checks, including extraction
    python3 regress.py --list-categories     # category names
    python3 regress.py --category save       # only the save checks
    python3 regress.py --category render,scene
    python3 regress.py --category film --full
    python3 regress.py --game "D:/games/Gunmetal 3dfx" --out /tmp/opencode/regress

Categories partition the quick suite exactly: every check belongs to one
category, so the category runs add up to the same 113/121 total.  ``--full``
enables the heavy checks inside the selected categories; the ``extract``
category implies it.  Exit code is 0 only when every selected check passes.
Nothing in the game directory is written; all output goes to ``--out``.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GM = os.path.join(HERE, "gm.py")
DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
FORCE = ["--force"]
T0 = time.time()

# A category groups checks with the same inputs and failure mode.  ``--category``
# runs only the selected groups; the default (empty set) still runs everything.
CATEGORIES = {
    "archives": "BIGFILE listings and the installation inventory",
    "trust": "malformed input, fuzz matrix and output guards",
    "config": "GUNMETAL.CFG and the opaque CONTROLS.CFG",
    "repack": "extract/pack/set on a shipped BIGFILE",
    "write": "in-process SBM writer round-trips",
    "render": "software renderer, culling and golden PNGs",
    "level": "SBW/SBT/SBC readers, rebuilds and residual fields",
    "audio": "SOU/SOU1/SOU2 decoders, music and SOG",
    "models": "SOB/SOE models, thing types and chassis",
    "anim": "SOO/SOA playback and 91 Hz timelines",
    "scene": "scripted things, starts, doors, switches and lifts",
    "video": "SBV container and pixel codec",
    "look": "LOOK16/LOOK162 tables",
    "save": "SAVE fields, writer, patching and savegame layout",
    "ui": "SML/SME markup, tags and screens",
    "film": "SOF/FDX films, tiles and APNG",
    "vm": "SBT SCRIPT VM and record writers",
    "campaign": "campaign/deathmatch transition map",
    "entities": "entity table and ADD_THING operands",
    "names": "engine address map",
    "misc": "small formats and CFG containers",
    "extract": "full texture/sprite/sound/frontend extraction (implies --full)",
}
ONLY: set[str] = set()


def want(*categories: str) -> bool:
    """True when *any* of the categories is in the selected scope."""
    return not ONLY or bool(ONLY.intersection(categories))


def stamp() -> str:
    """Wall-clock stamp, so a slow check is visible in the log."""
    return "t+%5.1fs" % (time.time() - T0)


def run_raw(args: list[str]) -> tuple[int, str]:
    p = subprocess.run(
        [sys.executable, GM, *args],
        capture_output=True, text=True, cwd=HERE,
    )
    return p.returncode, (p.stdout + p.stderr)


def run(args: list[str]) -> tuple[int, str]:
    # The suite owns its --out directory and is expected to be re-runnable, so
    # it explicitly consents to replacing previous results.
    return run_raw([*args, "--force"])


def config_checks(G: str, results: list[bool]) -> None:
    """GUNMETAL.CFG field map and the opaque CONTROLS.CFG container."""
    if not want("config"):
        return
    rc, text = run_raw(["misc", os.path.join(G, "GUNMETAL.CFG"), "--json"])
    rows = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
    row = rows[0] if rows else {}
    ok = (rc == 0 and row.get("width") == 800 and row.get("height") == 600
          and row.get("colour_depth") == 8
          and row.get("key_profiles") == ["PERFECT", "SPLIT1"]
          and row.get("opaque") is False)
    results.append(report("config / GUNMETAL.CFG fields", ok,
                          "800x600x8, keys PERFECT/SPLIT1"))

    rc, text = run_raw(["misc", os.path.join(G, "CONTROLS.CFG"), "--json"])
    rows = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
    row = rows[0] if rows else {}
    results.append(report("config / CONTROLS.CFG is opaque",
                          rc == 0 and row.get("opaque") is True
                          and str(row.get("version")) == "1.91",
                          "av1.91, unread by gundx.exe"))


def trust_checks(G: str, results: list[bool], out: str) -> None:
    """Trust-boundary checks: malformed input fails cleanly, outputs are guarded."""
    if not want("trust"):
        return
    audit = os.path.join(out, "audit")
    shutil.rmtree(audit, ignore_errors=True)
    os.makedirs(audit)

    def write(name: str, blob: bytes) -> str:
        path = os.path.join(audit, name)
        with open(path, "wb") as f:
            f.write(blob)
        return path

    # An index offset past EOF must be rejected before any read.
    idx = bytearray(open(os.path.join(G, "PICS", "SBM.IDX"), "rb").read())
    struct.pack_into("<I", idx, 20, 0xFFFFFF00)
    rc, text = run_raw(["list", write("bad.IDX", bytes(idx))])
    results.append(report("trust / out-of-range archive offset rejected",
                          rc != 0 and "Traceback" not in text, "exit 1, no traceback"))

    # Truncated geometry and impossible video dimensions fail with one line.
    with open(os.path.join(G, "LEVEL0.SBW"), "rb") as f:
        sbw_cut = f.read(100)
    rc, text = run_raw(["sbw", write("trunc.SBW", sbw_cut)])
    results.append(report("trust / truncated .sbw rejected",
                          rc != 0 and "Traceback" not in text, "exit 1, no traceback"))

    with open(os.path.join(G, "Vid", "Intro.sbv"), "rb") as f:
        sbv_blob = bytearray(f.read())
    struct.pack_into("<I", sbv_blob, 8, 0)          # height = 0
    rc, text = run_raw(["sbv", write("bad.sbv", bytes(sbv_blob)), "--list-only"])
    results.append(report("trust / impossible .sbv height rejected",
                          rc != 0 and "Traceback" not in text, "exit 1, no traceback"))

    # A crafted entry name must not escape the output directory.
    evil_name = b"../../X\0"
    evil = b"BIGFILE\x1a" + struct.pack("<I", 1) + evil_name + struct.pack("<I", 0)
    with open(os.path.join(audit, "evil.IDX"), "wb") as f:
        f.write(evil)
    with open(os.path.join(audit, "evil.DAT"), "wb") as f:
        f.write(b"DATA")
    evil_out = os.path.join(audit, "evil_out")
    rc, _ = run_raw(["extract", os.path.join(audit, "evil.IDX"), evil_out])
    escaped = os.path.exists(os.path.join(audit, "X.bin"))
    inside = os.path.join(evil_out, "______X.bin")
    results.append(report("trust / archive names cannot escape the output dir",
                          rc == 0 and not escaped and os.path.exists(inside),
                          os.path.basename(inside)))

    # Re-running an extraction must not silently clobber the first result.
    target = os.path.join(audit, "extract")
    rc1, _ = run_raw(["extract", os.path.join(G, "sprites", "SBM.IDX"), target])
    rc2, text2 = run_raw(["extract", os.path.join(G, "sprites", "SBM.IDX"), target])
    rc3, _ = run_raw(["extract", "--force", os.path.join(G, "sprites", "SBM.IDX"), target])
    results.append(report("trust / extract refuses silent overwrite",
                          rc1 == 0 and rc2 != 0 and "refusing to overwrite" in text2
                          and rc3 == 0, "needs --force"))

    # The full hostile-input matrix lives in its own tool; run the cheap pass.
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "fuzz_formats.py"),
                        "--game", G, "--quick"], capture_output=True, text=True, cwd=HERE)
    results.append(report("trust / malformed-input fuzz matrix",
                          p.returncode == 0, p.stdout.strip().splitlines()[0]
                          if p.stdout.strip() else "no output"))


def audio_checks(G: str, results: list[bool], out: str) -> None:
    """SOU container, SOU1/SOU2 parity and one end-to-end music export."""
    if not want("audio"):
        return

    # .sou container stats and rebuild.
    rc, text = run_raw(["sou", os.path.join(G, "sounds", "SOU.IDX"), "--json"])
    try:
        rep = json.loads(text)
    except Exception:
        rep = {}
    ok = (rc == 0 and rep.get("entries") == 999 and rep.get("bad") == 0
          and rep.get("formats") == {"SOU0": 13, "SOU1": 983, "SOU2": 3}
          and rep.get("loadable") == 999)
    results.append(report("sou / container stats (sounds)", ok,
                          "999: 13 SOU0 + 983 SOU1 + 3 SOU2, 0 bad"))
    # Python SOU1 decoder vs the emulator-verified FNV manifest.
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "parity_sou1.py"),
                        "--fast", "--game", G], capture_output=True, text=True, cwd=HERE)
    results.append(report("sou1 / Python decoder parity (fast)", p.returncode == 0,
                          (p.stdout.strip().splitlines() or ["no output"])[-1]))

    # Python SOU2 decoder vs the C++/engine reference (six pinned records).
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "parity_sou2.py"),
                        "--fast", "--game", G], capture_output=True, text=True, cwd=HERE)
    results.append(report("sou2 / Python port parity (fast)", p.returncode == 0,
                          (p.stdout.strip().splitlines() or ["no output"])[-1]))

    # End-to-end music export (one entry) must produce a playable SOU2 WAV.
    music_dir = os.path.join(out, "music_one")
    rc, text = run(["music", os.path.join(G, "MUSIC", "SOU.IDX"), music_dir,
                    "--limit", "1"])
    wav = os.path.join(music_dir, "DM1A01.wav")
    results.append(report("music / SOU2 WAV export",
                          rc == 0 and os.path.exists(wav)
                          and open(wav, "rb").read(4) == b"RIFF",
                          "DM1A01.wav"))


def render_variant_checks(results: list[bool]) -> None:
    """Renderer mip/bilinear/alpha variants keep the default golden hashes."""
    if not want("render"):
        return
    p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "parity_mip.py")],
                       capture_output=True, text=True, cwd=HERE)
    results.append(report("render / mip+bilinear+alpha variants",
                          p.returncode == 0,
                          (p.stdout.strip().splitlines() or ["no output"])[-1]))


def anim_smoke_checks(G: str, results: list[bool], out: str) -> None:
    """SOO playback/morphing; the SOA timeline lives in main()."""
    if not want("anim"):
        return

    # .soo playback: mode 1 wraps, frame_at mirrors the engine interpolator.
    rc, text = run_raw(["soo", os.path.join(G, "Sobanim", "Cultist.soo"),
                        "--timeline", "--at", "600"])
    ok = (rc == 0 and "mode=loop count=4" in text
          and "[    0..50   ) 0 cult1a       -> 1" in text
          and "total=400 ticks" in text
          and "at 600 ms (tick 54): frame 1 cult1a alpha=0.027 -> cult1b" in text)
    results.append(report("soo / loop timeline + interpolated pose", ok,
                          "Cultist: 4 frames / 400 ticks (4.40 s), wrap to 0"))
    a_png = os.path.join(out, "soo_at0.png")
    b_png = os.path.join(out, "soo_at100.png")
    rc1, _ = run(["view", os.path.join(G, "Sobanim", "Cultist.soo"),
                  "--game", G, "--at", "0", "--size", "160", "--out", a_png])
    rc2, text2 = run(["view", os.path.join(G, "Sobanim", "Cultist.soo"),
                      "--game", G, "--at", "1000", "--size", "160", "--out", b_png])
    different = (rc1 == 0 and rc2 == 0 and os.path.exists(a_png)
                 and os.path.exists(b_png)
                 and open(a_png, "rb").read() != open(b_png, "rb").read())
    results.append(report("soo / morphing pose render", different
                          and "anim mode=1" in text2,
                          "Cultist frame 0 vs 1000 ms differ"))


def asset_self_checks(G: str, results: list[bool]) -> None:
    """Small self-tests grouped by the asset family they cover."""
    if want("models"):
        # .soe/THING_TYPES field map: 77 fields, 36 proven by loader/consumers.
        rc, text = run_raw(["soe", "--fields"])
        ok = (rc == 0 and "soe fields: 77" in text
              and "proven 36" in text and "0x1a4" in text)
        results.append(report("soe / THING_TYPES field map", ok,
                              "77 fields: 36 proven, 25 hypothesis, 16 unknown"))

        # .sob writer: 672/672 semantic + raw round-trip, canonical writer.
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "sob_writer.py"),
                            "--self-test"], capture_output=True, text=True, cwd=HERE)
        results.append(report("sob / writer self-test", p.returncode == 0,
                              (p.stdout.strip().splitlines() or ["no output"])[-1]))

    if want("ui"):
        # SML/SME semantics: 300 files, 147 pairs, screen/asset tables.
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "ui_semantics.py"),
                            "--check"], capture_output=True, text=True, cwd=HERE)
        results.append(report("sml / UI semantics (300 files)",
                              p.returncode == 0 and "check: OK" in p.stdout,
                              "131 identical pairs, 14 revisions, 2 line-ending cases"))

    if want("scene"):
        # Shake/recoil formulas are pure math; the self-test needs no game data.
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "shake.py"),
                            "--selftest"], capture_output=True, text=True, cwd=HERE)
        results.append(report("shake / view-pitch and recoil model",
                              p.returncode == 0 and "40/40" in p.stdout,
                              "triangle wave, clamp, impulse/decay, recoil"))

        # Recursive/executed scene things (opt-in; default counts unchanged).
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "nested_things.py"),
                            "--self-test"], capture_output=True, text=True, cwd=HERE)
        results.append(report("scene / nested things model", p.returncode == 0
                              and "15 tests, 0 failed" in p.stdout,
                              "4008 top-level / 7563 recursive / 7617 executed"))

    if want("save"):
        # Save fields: 2 shipped saves, 83 proven / 31 hypothesis / 14 unknown.
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "save_fields.py"),
                            "--check"], capture_output=True, text=True, cwd=HERE)
        results.append(report("save / field map and shipped saves",
                              p.returncode == 0 and "2/2 files clean" in p.stdout,
                              "83 proven, 31 hypothesis, 14 unknown"))

    if want("vm"):
        # SCRIPT VM executor and structural .sbt record writers.
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "vm_exec.py"),
                            "--selftest"], capture_output=True, text=True, cwd=HERE)
        results.append(report("vm / executor self-test", p.returncode == 0
                              and "17 tests, 0 failed" in p.stdout,
                              "17 tests: IF/ELSE, GENERATE, limits, unknown opcode"))
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "records.py"),
                            "--check"], capture_output=True, text=True, cwd=HERE)
        results.append(report("sbt / all record writers", p.returncode == 0
                              and "38/38 file(s) byte-exact" in p.stdout,
                              "38 files, 289 records byte-exact"))
        # SECTOR_STUFF/ITEM/LIFT field round-trip and trust edge cases.
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re", "records.py"),
                            "--self-test"], capture_output=True, text=True, cwd=HERE)
        results.append(report("records / SECTOR_STUFF+ITEM+LIFT fields",
                              p.returncode == 0,
                              (p.stdout.strip().splitlines() or ["no output"])[-1]))

    if want("video"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "sbv_codec.py"),
                            "--self-test"], capture_output=True, text=True, cwd=HERE)
        results.append(report("sbv / pixel codec self-test", p.returncode == 0
                              and "26/26 self-test" in p.stdout,
                              "YUV planes, entropy blocks, 26 trust cases"))
        rc, text = run_raw(["sbv", os.path.join(G, "Vid", "Intro.sbv"),
                            "--verify", "--frames", "0"])
        results.append(report("sbv / frame encode/decode", rc == 0
                              and "pixel round-trip 1/1" in text,
                              "Intro frame 0 planes exact"))


# --- process-pool worker for the 36-level sweeps ---------------------------
# scene.load() is CPU-bound (parse + VM decode), so threads do not help.  All
# three sweeps (counts, sprite/geometry ratio, ADD_THING things) need the same
# scene.load(), so one worker returns all of them: the levels are parsed once
# instead of three times.
def _scan_level(path: str) -> tuple:
    """counts, sprite/geometry spread ratio and things for one level."""
    import statistics

    from gm import sbw, scene
    try:
        sc = scene.load(path)
    except Exception:  # noqa: BLE001 - the caller counts this as a failure
        return path, None, None, (0, 0, [])
    try:
        counts = sc.counts()
    except Exception:  # noqa: BLE001
        counts = None
    things = (len(sc.things), sum(1 for t in sc.things if t.absolute),
              [t.model for t in sc.things if t.model])

    ratio = None
    try:
        spr = sc.sprites
        if len(spr) >= 4:
            wb = sbw.parse_file(os.path.splitext(path)[0] + ".sbw")
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
            pspan = [max(q.pos[k] for q in spr) - min(q.pos[k] for q in spr)
                     for k in range(3)]
            r = [pspan[k] / gspan[k] for k in range(3) if gspan[k] > 1]
            ratio = statistics.median(r) if r else None
    except Exception:  # noqa: BLE001 - levels without a usable sibling .sbw
        ratio = None
    return path, counts, ratio, things


def _pool_map(fn, files: list[str], workers: int = 0) -> list:
    from concurrent.futures import ProcessPoolExecutor
    # 16 is the sweet spot on the 32-core dev box: 8 leaves half the machine
    # idle, 32 buys <15% more and multiplies the workers' RSS.
    workers = workers or min(os.cpu_count() or 8, 16)
    workers = max(1, min(workers, len(files), os.cpu_count() or 1))
    with ProcessPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(fn, files))


def check_result(name: str, args: list[str], expect: list[str] | None,
                 rc: int, out: str) -> bool:
    tail = out.strip().splitlines()[-1] if out.strip() else "(no output)"
    ok = rc == 0
    for e in expect or []:
        if e not in out:
            ok = False
    status = "ok  " if ok else "FAIL"
    print(f"[{status}] {name}  [{stamp()}]")
    if not ok:
        print(f"        cmd: gm.py {' '.join(args)}")
        print(f"        last: {tail}")
        for e in expect or []:
            if e not in out:
                print(f"        missing: {e!r}")
    return ok


def check(name: str, args: list[str], expect: list[str] | None = None) -> bool:
    rc, out = run(args)
    return check_result(name, args, expect, rc, out)


def check_many(specs: list[tuple], workers: int = 4) -> list[bool]:
    """Run independent checks concurrently, print them in submission order.

    The extraction checks (textures/sprites/sounds/frontend) are independent
    gm.py invocations and dominate ``--full``; with 32 cores there is no reason
    to serialise them.
    """
    from concurrent.futures import ThreadPoolExecutor
    results: list[bool] = []

    def work(spec):
        name, args, expect = spec
        rc, out = run(args)
        return name, args, expect, rc, out

    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        for name, args, expect, rc, out in pool.map(work, specs):
            results.append(check_result(name, args, expect, rc, out))
    return results


def report(name: str, ok: bool, detail: str = "") -> bool:
    print(f"[{'ok  ' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if ok and detail else "")
          + f"  [{stamp()}]")
    if not ok and detail:
        print(f"        {detail}")
    return ok


def render_checks(G: str, results: list[bool], out: str, full: bool = False) -> None:
    """Model/level rendering: deterministic PNGs with a sane coverage."""
    if not want("render"):
        return
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
    if not want("video"):
        return
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


def scene_checks(G: str, results: list[bool], out: str,
                 scans: list | None = None) -> None:
    """Scripted scene extraction (gm.scene) and its viewer overlay.

    The shipped levels carry no placed-object list at all, so the scene is
    decoded out of the compiled ``SCRIPT`` code; these checks pin the operand
    layouts down to the engine handlers (see the gm/scene.py docstring) and make
    sure the viewer overlay stays wired to them.

    ``scans`` is the shared ``_scan_level`` sweep (counts, ratio, things) that
    ``main`` already needed for the ADD_THING check; it is computed here when
    the caller has none.
    """
    if not want("scene"):
        return
    sys.path.insert(0, HERE)
    from gm import render, sbt, sbtvm, sbw, scene

    files = sorted(glob.glob(os.path.join(G, "*.sbt")))
    if scans is None:
        scans = _pool_map(_scan_level, files)

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
    # Player starts: SET_PLAYER_START (169) / SET_DEATHMATCH_START (168), laid
    # out as slot,x,y,z + three f64 angles (docs/PLAYER.md).
    s0 = scene.load(os.path.join(G, "level0.sbt"))
    starts0 = [s for s in s0.spawns if s.kind == "campaign" and s.index == 0]
    eye0, look0, basis0 = scene.first_person(starts0[0]) if starts0 else (None, None, {})
    ok_start = (len(s0.spawns) == 8 and len(starts0) == 1
                and starts0[0].pos == (24, -208, 48)
                and starts0[0].angles == (0.0, 0.0, 0.0)
                and eye0 == (24, -208, 58)
                and basis0.get("forward") == (0.0, 1.0, -0.0))
    results.append(report("scene / player start + first-person basis", ok_start,
                          "slot 0 (24,-208,48), eye z+10, forward +Y"))
    dm = scene.load(os.path.join(G, "dm1.sbt"))
    campaign = sum(1 for s in dm.spawns if s.kind == "campaign")
    deathmatch = sum(1 for s in dm.spawns if s.kind == "deathmatch")
    results.append(report("scene / deathmatch starts (dm1)",
                          campaign == 5 and deathmatch == 8,
                          "dm1: %d campaign %d deathmatch" % (campaign, deathmatch)))

    # ADD_SKY_LIGHT_SOURCE is stored as (x, y, z, param, r, g, b) by 0x47e130
    sky = [l for l in sc.lights if l.kind == "sky"]
    results.append(report(
        "scene / proven sky-light operands",
        len(sky) == 2 and sky[0].pos == (-1040, -2568, 3526)
        and sky[0].rgb == (255, 255, 255) and sky[0].param == 112,
        "sky lights: " + ", ".join(f"{l.pos} rgb={l.rgb}" for l in sky)))

    tot = {"lights": 0, "locations": 0, "items": 0, "sounds": 0, "sprites": 0}
    bad = 0
    for _f, c, _r, _t in scans:
        if c is None:
            bad += 1
            continue
        for k in tot:
            tot[k] += c[k]
    results.append(report(
        "scene / all levels decode",
        bad == 0 and tot["lights"] == 4470 and tot["locations"] == 461
        and tot["sounds"] == 1173 and tot["sprites"] == 555,
        "%d levels: %d lights, %d locations, %d items, %d sounds, %d sprites, "
        "%d failed" % (len(scans), tot["lights"], tot["locations"], tot["items"],
                       tot["sounds"], tot["sprites"], bad)))

    # Coordinate frame: scripted coordinates are engine world, i.e. 1.875x the
    # .sbw file frame (the object header basis).  Unlike the biased "inside the
    # AABB" test, this spread ratio is scale-covariant: sprite positions are
    # props that must sit inside their level, so the ratio has to come out as
    # the basis itself.
    import statistics

    ratios = [r for _f, _c, r, _t in scans if r is not None]
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
    """In-process round-trip checks for the writers (sbm blobs, level records)."""
    if not want("write", "level", "vm", "models", "anim", "audio"):
        return
    sys.path.insert(0, HERE)

    from gm import sbt as sbtmod
    sbt_paths = sorted(glob.glob(os.path.join(G, "*.sbt")))

    if want("write"):
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
        results.append(report("sbm / blob round-trip", bad == 0,
                              f"{n} blobs byte-identical"))

        bf = bigfile.BigFile(os.path.join(G, "sprites/SBM.IDX"))
        dat = open(bf.dat_path, "rb").read()
        bad = 0
        sample = bf.entries[:48]
        for e in sample:
            blob = e.data(dat)
            s = sbm.parse(blob)
            img = png.decode(s.to_png(pal))      # indexed PNG preserves indices
            re = sbm.encode(s.width, s.height, img.pixels, levels=s.levels, flags=s.flags)
            if re != blob:
                bad += 1
        results.append(report("sbm / png->sbm round-trip", bad == 0,
                              f"{len(sample)} sprites byte-identical"))

    if want("level"):
        # sbw: re-serialise every level and compare byte-for-byte
        rb = os.path.join(out, "sbw_rebuild")
        run(["sbw", *sorted(glob.glob(os.path.join(G, "*.sbw"))), "--rebuild", rb])
        same = True
        for p in sorted(glob.glob(os.path.join(G, "*.sbw"))):
            rp = os.path.join(rb, os.path.basename(p))
            if not os.path.exists(rp) or open(rp, "rb").read() != open(p, "rb").read():
                same = False
                break
        results.append(report("sbw / rebuild round-trip", same,
                              "36 files byte-identical"))

        # sbt: re-serialise + structured re-encode of the modelled records
        rb = os.path.join(out, "sbt_rebuild")
        run(["sbt", G, "--rebuild", rb])
        same = True
        for p in sbt_paths:
            rp = os.path.join(rb, os.path.basename(p))
            if not os.path.exists(rp) or open(rp, "rb").read() != open(p, "rb").read():
                same = False
                break
        results.append(report("sbt / rebuild round-trip", same,
                              f"{len(sbt_paths)} files byte-identical"))

        enc_ok = enc_tot = 0
        for p in sbt_paths:
            s = sbtmod.parse(open(p, "rb").read())
            for rec in s.records:
                if rec.type in (4, 5, 15, 16, 17, 20):
                    enc_tot += 1
                    if sbtmod.encode_payload(rec.type, rec.payload) == rec.raw:
                        enc_ok += 1
        results.append(report("sbt / structured re-encode",
                              enc_ok == enc_tot and enc_tot > 0,
                              f"{enc_ok}/{enc_tot} records exact"))

    if want("vm"):
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
        results.append(report("sbt / SCRIPT VM decode",
                              dec_ok == dec_tot and dec_tot > 0,
                              f"{dec_ok}/{dec_tot} code blocks decode to exact end"))

    # text/binary descriptor writers: rebuild every shipped file
    from gm import sbc as sbcmod, soa as soamod, sog as sogmod, soe as soemod

    def all_ext(root: str, ext: str) -> list[str]:
        found = []
        for r, _d, fs in os.walk(root):
            found += [os.path.join(r, f) for f in fs if f.lower().endswith(ext)]
        return sorted(found)

    cases = [
        ("sbc", ".sbc", sbcmod.parse, "level"),
        ("soa", ".soa", soamod.parse, "anim"),
        ("sog", ".sog", sogmod.parse, "audio"),
        ("soe", ".soe", lambda d: soemod.parse_text(d.decode("latin1")), "models"),
    ]
    for label, ext, parsefn, category in cases:
        if not want(category):
            continue
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
    if not want("ui"):
        return
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
    if not want("level"):
        return
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


def save_checks(G: str, results: list[bool]) -> None:
    """Every installed save must yield its player gauges and its record counts."""
    if not want("save"):
        return
    tool = os.path.join(HERE, "tools", "re", "savefile.py")
    saves = sorted(glob.glob(os.path.join(G, "SAVEGAME", "*.sbt")))
    if not saves:
        results.append(report("save / player gauges", True, "no saves installed"))
        return
    bad, seen = [], []
    for path in saves:
        p = subprocess.run([sys.executable, tool, path, "--game", G, "--json"],
                           capture_output=True, text=True, cwd=HERE)
        try:
            info = json.loads(p.stdout)
        except Exception:
            bad.append(os.path.basename(path) + ": unparsable")
            continue
        pl = info.get("player")
        if not pl:
            bad.append(os.path.basename(path) + ": no player block")
            continue
        if not 0 < pl["armor"] <= pl["armor_max"]:
            bad.append(os.path.basename(path) + ": armor out of range")
        elif not all(0 <= v <= pl["shield_max"] for v in pl["shields"].values()):
            bad.append(os.path.basename(path) + ": shield out of range")
        elif any(v > 1000000 for v in pl["ammo"].values()):
            bad.append(os.path.basename(path) + ": ammo out of range")
        elif any({int(k): v for k, v in pl["ammo"].items()}.get(i, 0) > cap
                 for i, cap in enumerate(pl.get("ammo_max") or [])
                 if cap and pl["weapon_mask"] >> i & 1):
            bad.append(os.path.basename(path) + ": ammo above its limit")
        elif info.get("doors") and len(info["doors"]["records"]) != info["level_doors"]:
            bad.append(os.path.basename(path) + ": door records")
        else:
            used = sum(1 for v in pl["ammo"].values() if v)
            seen.append("%s=%.2f/%d wpn" % (os.path.basename(path)[:-4],
                                           pl["armor"], used))
    results.append(report("save / player gauges + ammo", not bad,
                          ", ".join(seen) if seen else "none"))
    if bad:
        print("        " + "; ".join(bad))
    if saves and not bad:
        p = subprocess.run([sys.executable, GM, "save", saves[0], "--game", G],
                           capture_output=True, text=True, cwd=HERE)
        results.append(report("save / gm.py save", p.returncode == 0
                              and "player:" in p.stdout,
                              os.path.basename(saves[0])))
        p = subprocess.run([sys.executable, GM, "save", saves[0], "--game", G,
                            "--rebuild"], capture_output=True, text=True, cwd=HERE)
        results.append(report("save / rebuild is byte-exact",
                              p.returncode == 0 and "byte-exact" in p.stdout,
                              os.path.basename(saves[0])))
        import json as _json
        info = _json.loads(subprocess.run(
            [sys.executable, tool, saves[0], "--game", G, "--json"],
            capture_output=True, text=True, cwd=HERE).stdout)
        chain = info.get("chain") or []
        pl = info.get("player") or {}
        ok = bool(chain) and chain[-1]["id"] == 9 and chain[-1]["offset"] == pl.get("offset")
        results.append(report("save / block chain ends on the player", ok,
                              " -> ".join(str(b["id"]) for b in chain)))
        out = os.path.join("/tmp/opencode", "regress_patched.sbt")
        p = subprocess.run([sys.executable, GM, "save", saves[0], "--game", G,
                            "--set", "armor=50", "--set", "ammo=0:1234",
                            "--set", "door=0:1", "--out", out, "--force"],
                           capture_output=True, text=True, cwd=HERE)
        back = _json.loads(subprocess.run(
            [sys.executable, tool, out, "--game", G, "--json"],
            capture_output=True, text=True, cwd=HERE).stdout)
        bp = back.get("player") or {}
        ok = (p.returncode == 0 and abs(bp.get("armor", 0) - 50.0) < 1e-9
              and str(bp.get("ammo", {}).get(0, bp.get("ammo", {}).get("0"))) == "1234"
              and (back.get("doors") or {}).get("records", [{}])[0].get("state") == 1)
        results.append(report("save / --set writes a patched copy", ok, out))
        p = subprocess.run([sys.executable, GM, "save", saves[0], "--game", G,
                            "--set", "ammo=0:999999", "--out", out + ".bad", "--force"],
                           capture_output=True, text=True, cwd=HERE)
        results.append(report("save / --set rejects ammo above the limit",
                              p.returncode != 0 and not os.path.exists(out + ".bad")))
        p = subprocess.run([sys.executable, GM, "save", saves[0], "--game", G,
                            "--diff", saves[-1]], capture_output=True,
                           text=True, cwd=HERE)
        results.append(report("save / diff attributes differences to blocks",
                              p.returncode == 0
                              and "state blocks" in p.stdout
                              and "9 player" in p.stdout
                              and "2 doors" in p.stdout,
                              "per-block byte counts"))


def inventory_check(G: str, results: list[bool]) -> None:
    """The installation inventory sees every format and BIGFILE container."""
    inv = subprocess.run([sys.executable, GM, "inventory", "--game", G, "--json"],
                         capture_output=True, text=True, cwd=HERE)
    try:
        data = json.loads(inv.stdout)
    except Exception:
        data = {}
    exts = {r["ext"] for r in data.get("rows", [])}
    cont = {c["idx"].split("/")[0]: c["entries"] for c in data.get("containers", [])}
    ok = (inv.returncode == 0 and {".sbw", ".sbt", ".sob", ".dat"} <= exts
          and cont.get("sounds") == 999 and cont.get("MUSIC") == 3092)
    results.append(report("inventory covers the installation and its BIGFILEs",
                          ok, "%d files, %d containers"
                          % (data.get("files", 0), len(data.get("containers", [])))))


def main() -> int:
    ONLY.clear()
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME, help="Gunmetal installation directory")
    ap.add_argument("--out", help="output directory for extraction checks")
    ap.add_argument("--full", action="store_true",
                    help="also extract textures/sounds/sprites/frontend (slow, writes many files)")
    ap.add_argument("--category", "--only", action="append", default=[],
                    metavar="CAT",
                    help="run only these categories (repeatable or comma-separated); "
                         "see --list-categories")
    ap.add_argument("--list-categories", action="store_true",
                    help="print the category names and exit")
    opts = ap.parse_args()

    if opts.list_categories:
        width = max(len(name) for name in CATEGORIES)
        for name, description in CATEGORIES.items():
            print(f"  {name:<{width}}  {description}")
        return 0

    selected: list[str] = []
    for item in opts.category:
        selected.extend(part.strip() for part in item.split(",") if part.strip())
    unknown = [name for name in selected if name != "all" and name not in CATEGORIES]
    if unknown:
        ap.error("unknown category: " + ", ".join(unknown)
                 + " (use --list-categories)")
    if "all" not in selected:
        ONLY.update(selected)
    if "extract" in ONLY:
        opts.full = True

    G = opts.game
    out = opts.out or os.path.join(tempfile.gettempdir(), "opencode", "regress")
    os.makedirs(out, exist_ok=True)
    results: list[bool] = []
    if ONLY:
        print("regress: categories " + ", ".join(sorted(ONLY))
              + (" (--full)" if opts.full else ""))

    def sbw_files() -> list[str]:
        return sorted(glob.glob(os.path.join(G, "*.sbw")))

    # --- archives ---------------------------------------------------------
    if want("archives"):
        results.append(check("archives / PICS",     ["list", f"{G}/PICS/SBM.IDX"],     ["1648 entries"]))
        results.append(check("archives / sprites",  ["list", f"{G}/sprites/SBM.IDX"],  ["179 entries"]))
        results.append(check("archives / sounds",   ["list", f"{G}/sounds/SOU.IDX"],   ["999 entries"]))
        results.append(check("archives / frontend", ["list", f"{G}/FRONTEND/BMP.IDX"], ["143 entries"]))
        results.append(check("archives / music",    ["list", f"{G}/MUSIC/SOU.IDX"],    ["3092 entries"]))
        inventory_check(G, results)

    trust_checks(G, results, out)
    audio_checks(G, results, out)
    anim_smoke_checks(G, results, out)
    render_variant_checks(results)
    asset_self_checks(G, results)
    config_checks(G, results)

    # --- BIGFILE write path: extract -> pack must reproduce the archive ----
    if want("repack"):
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
    if want("level"):
        results.append(check("sbw / all levels", ["sbw", *sbw_files()],
                             ["sbw: 36 parsed, 0 failed"]))
        results.append(check("sbt / all levels + resolve", ["sbt", G, "--resolve"],
                             ["sbt: 36 parsed, 0 failed"]))
        results.append(check("sbc / all levels",
                             ["sbc", *sorted(glob.glob(os.path.join(G, "*.sbc"))), out]
                             + ([] if opts.full else ["--check"]),
                             ["sbc: 36 files, 0 failed"]))

    # --- models / animations / audio -------------------------------------
    if want("models"):
        results.append(check("sob / models", ["sob", f"{G}/SOBS", out]
                             + ([] if opts.full else ["--check"]),
                             ["sob: 672 converted, 0 failed"] if opts.full
                             else ["sob: 672 parsed, 0 failed"]))
    if opts.full and want("models"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "sob_writer.py"), "--check"],
                           capture_output=True, text=True, cwd=HERE)
        results.append(report("sob / 672-model writer round-trip",
                              p.returncode == 0 and "672/672 identical" in p.stdout
                              and "byte-exact via raw        : 672/672" in p.stdout,
                              "semantic + raw byte-exact"))
    if opts.full and want("audio"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "sou_container.py"), "--check"],
                           capture_output=True, text=True, cwd=HERE)
        results.append(report("sou / 4091-entry rebuild", p.returncode == 0
                              and "4091/4091" in p.stdout,
                              (p.stdout.strip().splitlines() or ["no output"])[-1]))
    if opts.full and want("vm"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "vm_exec.py"), "--corpus",
                            "--compare"], capture_output=True, text=True, cwd=HERE)
        results.append(report("vm / 36-level execution vs scene", p.returncode == 0
                              and "spawns" in p.stdout,
                              "all main scripts reach END; 391 spawns match"))
    render_checks(G, results, out, opts.full)
    # One shared sweep feeds "all levels decode", the 1.875 world-frame ratio
    # and the ADD_THING check below.
    level_scans = None
    if want("scene"):
        level_scans = _pool_map(_scan_level,
                                sorted(glob.glob(os.path.join(G, "*.sbt"))))
    scene_checks(G, results, out, level_scans)
    if want("anim"):
        results.append(check("soa / animations", ["soa", G],
                             ["soa: 104 parsed, 0 failed"]))
        rc, text = run_raw(["soa", os.path.join(G, "anims", "pulsar.soa"),
                            "--timeline", "--at", "100"])
        results.append(report("soa / 91 Hz timeline + frame_at",
                              rc == 0 and "period=32 (29 ticks)" in text
                              and "at 100 ms (tick 9): frame 7 pulsar8" in text,
                              "pulsar: 29 ticks, t=9 -> frame 7"))
        results.append(check("soo / animations", ["soo", f"{G}/Sobanim"],
                             ["soo: 40 parsed, 0 failed, 8 frame names without a .sob"]))
        rb = os.path.join(out, "soo")
        run(["soo", f"{G}/Sobanim", "--rebuild", rb])
        src = sorted(glob.glob(os.path.join(G, "Sobanim", "*.[sS][oO][oO]")))
        same = bool(src) and all(
            os.path.exists(os.path.join(rb, os.path.basename(p)))
            and open(os.path.join(rb, os.path.basename(p)), "rb").read()
            == open(p, "rb").read() for p in src)
        results.append(report("soo / 40 byte-exact round-trips", same,
                              "Sobanim -> DIR"))
    if want("film"):
        p = subprocess.run([sys.executable, GM, "film", f"{G}/Film", "--json"],
                           capture_output=True, text=True, cwd=HERE)
        rows = [json.loads(line) for line in p.stdout.splitlines()
                if line.startswith("{")]
        ok = (p.returncode == 0 and len(rows) == 48
              and all(r["bytes"] % 8192 == 0 and r["tiles"] > 0 for r in rows))
        results.append(report("film / 48 .sof are 8 KB tile-aligned", ok,
                              "%d films, %d tiles" % (len(rows),
                                                      sum(r["tiles"] for r in rows))))
        film_png = os.path.join(out, "film.png")
        run(["film", f"{G}/Film/AMMO4.SOF", "--frame", "0", "--png", film_png] + FORCE)
        results.append(report("film / tile export", os.path.exists(film_png)
                              and os.path.getsize(film_png) > 0, film_png))
        rc, text = run_raw(["film", os.path.join(G, "Film", "AMMO4.SOF"), "--json"])
        rows = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
        body = (rows[0].get("binding") or {}) if rows else {}
        ok = (rc == 0 and rows and rows[0].get("tiles") == 132
              and body.get("family") == "AMMO" and body.get("id") == 4
              and body.get("count") == 131 and body.get("fps") == 24
              and body.get("extra_tiles") == 1)
        results.append(report("film / family/index binding", ok,
                              "AMMO id=4, 131@24, one trailing tile"))
        film_src = os.path.join(G, "Film", "AMMO4.SOF")
        film_out = os.path.join(out, "film_rebuild")
        rc, text = run(["film", film_src, "--rebuild", film_out])
        same = (os.path.exists(os.path.join(film_out, "AMMO4.SOF"))
                and os.path.exists(os.path.join(film_out, "ammo.fdx")))
        if same:
            with open(film_src, "rb") as a, \
                    open(os.path.join(film_out, "AMMO4.SOF"), "rb") as b:
                same = a.read() == b.read()
            with open(os.path.join(G, "Film", "ammo.fdx"), "rb") as a, \
                    open(os.path.join(film_out, "ammo.fdx"), "rb") as b:
                same = same and a.read() == b.read()
        results.append(report("film / byte-exact rebuild", rc == 0 and same,
                              "AMMO4.SOF + ammo.fdx"))
        apng = os.path.join(out, "ammo4.apng")
        rc, text = run(["film", film_src, "--apng", apng, "--limit-frames", "4"])
        ok = False
        if rc == 0 and os.path.exists(apng):
            blob = open(apng, "rb").read()
            ok = blob[:8] == b"\x89PNG\r\n\x1a\n" and blob.count(b"fcTL") == 4 \
                and b"acTL" in blob
        results.append(report("film / APNG export", ok, "AMMO4: 4 frames"))
    if want("misc"):
        misc_dir = os.path.join(out, "misc")
        rc, text = run(["misc", G, "--rebuild", misc_dir] + FORCE)
        srcs = [os.path.join(G, p) for p in (
            "PICS/textures.sub", "sprites/sprites.sub", "sounds/sounds.sub",
            "FRONTEND/KILLED.skk", "FRONTEND/KILLED.skb", "path.sni",
            "KEYCFG/PERFECT.key", "KEYCFG/SPLIT1.KEY", "KEYCFG/SPLIT2.KEY",
            "gunmetal.hi", "GUNMETAL.CFG", "CONTROLS.CFG")]
        same = all(os.path.exists(os.path.join(misc_dir, os.path.basename(p)))
                   and open(os.path.join(misc_dir, os.path.basename(p)), "rb").read()
                   == open(p, "rb").read() for p in srcs)
        results.append(report("misc / 10 small formats round-trip", rc == 0 and same,
                              "sub/kk/kb/sni/key/hi"))
    if want("models"):
        results.append(check("soe / thing types", ["soe", G],
                             ["soe: 1 parsed, 0 failed"]))
    if want("audio"):
        results.append(check("sog / songs",
                             ["sog", f"{G}/MUSIC", "--music", f"{G}/MUSIC/SOU.IDX"],
                             ["sog: 651 songs"]))

    # --- video / lookup tables -------------------------------------------
    if want("video"):
        results.append(check("sbv / Intro header",
                             ["sbv", f"{G}/Vid/Intro.sbv", "--list-only"]))
        results.append(check("sbv / orient header",
                             ["sbv", f"{G}/Vid/orient.sbv", "--list-only"]))
        results.append(check("sbv / winner header",
                             ["sbv", f"{G}/Vid/winner.sbv", "--list-only"]))
    sbv_checks(G, results, out)
    if want("look"):
        results.append(check("look / LOOK16", ["look", f"{G}/LOOK16.TAB"],
                             ["234 distinct"]))
        results.append(check("look / LOOK162 vs NEONS.PIN",
                             ["look", f"{G}/LOOK162.TAB", "--neons", f"{G}/NEONS.PIN"],
                             ["20 distinct", "value-set match: True"]))
    # --- residual .sbw fields (fast: parses all 36 levels in ~2 s) --------
    sbw_field_checks(G, results)
    save_checks(G, results)

    # --- .sml/.sme markup -------------------------------------------------
    sml_checks(G, results)
    if want("ui"):
        results.append(check("sml / extract markup + text",
                             ["sml", f"{G}/SML", os.path.join(out, "sml")],
                             ["sml: 300 files (151 decoded from .sme, 1 repaired)"]))

    # --- campaign / deathmatch transition map (tools/re/campaign.py) ------
    if want("campaign"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "campaign.py"),
                            "--game", G], capture_output=True, text=True, cwd=HERE)
        rows = [l for l in p.stdout.splitlines()[1:] if l[:1].isalpha()]
        ok = (p.returncode == 0 and len(rows) == 36
              and "DM5 - TEMPLE" in p.stdout and "secrt1" in p.stdout)
        results.append(report("campaign / transition map", ok,
                              f"{len(rows)} levels, secrt1+temple present"))

    # --- chassis -> craft model (tools/re/chassis_models.py) --------------
    if want("models"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "chassis_models.py"),
                            "--game", G], capture_output=True, text=True, cwd=HERE)
        last = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""
        ok = p.returncode == 0 and "8 names in SOB_TABLE: 36/36" in last \
            and "CHEVY: 0/36" in last
        results.append(report("chassis / craft model resolution", ok,
                              last or "no output"))

    # --- thing type -> model (tools/re/thing_models.py) -------------------
    if want("models"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "thing_models.py"),
                            "--game", G], capture_output=True, text=True, cwd=HERE)
        last = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""
        ok = p.returncode == 0 and "valid SOB index 2896/2896" in last
        results.append(report("things / type -> model (+0x1a4)", ok,
                              last or "no output"))

    # --- entity table -> sub-script -> thing type (tools/re/entities.py) -----
    if want("entities"):
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
    # --- engine address map (tools/re/names.csv) ---------------------------
    if want("names"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "names.py")],
                           capture_output=True, text=True, cwd=HERE)
        ok = (p.returncode == 0 and "entries in" in p.stdout
              and "FAIL" not in p.stderr)
        results.append(report("names / engine address map validated", ok,
                              p.stdout.strip().splitlines()[0]
                              if p.stdout else "no output"))

    # --- things built by the entity sub-scripts (gm/scene.py) --------------
    if want("scene"):
        n_things = n_abs = n_models = 0
        models: set = set()
        for _p, _c, _r, (n, n_a, names) in level_scans or []:
            n_things += n
            n_abs += n_a
            models.update(names)
            n_models += len(names)
        results.append(report("scene / things (ADD_THING* in sub-scripts)",
                              n_things == 4008 and n_abs == 2844
                              and n_models == 4005 and len(models) == 138,
                              "%d things (%d placed, %d models, %d with a model)"
                              % (n_things, n_abs, len(models), n_models)))

    # --- .sbw object header transform (gm/sbw.py, gm/render.py) -----------
    lvl = None
    if want("level", "scene"):
        from gm import sbw as sbwmod
        lvl = sbwmod.parse_file(os.path.join(G, "dm1.sbw"))
    if want("level"):
        from gm import render as rendermod
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
            uv_ok = (abs(uv_a[0] - uv_b[0]) < 0.5
                     and abs(uv_a[1] - uv_b[1]) < 0.5)
        results.append(report(
            "sbw / object header transform",
            ok_tf == len(offs) and uv_ok and max(offs) < 16,
            "%d objects: world = origin + basis*local, max |origin| %.1f, "
            "UV kept: %s" % (len(offs), max(offs), uv_ok)))

    # --- savegame layout (tools/re/savegame.py) ---------------------------
    if want("save"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "re",
                                                         "savegame.py")],
                           capture_output=True, text=True, cwd=HERE)
        sg_out = p.stdout      # NB: not ``out`` — that is the output directory
        results.append(report(
            "savegame / level pair + state dispatcher",
            p.returncode == 0
            and "save is a level pair: savegame\\saved%d.sbt + savegame\\saved%d.sbw" in sg_out
            and "20 ids, 18 distinct handlers" in sg_out
            and "id   1  switches       0x44f000" in sg_out
            and "id   2  doors          0x44e540" in sg_out
            and "id  18  lifts          0x44ec20" in sg_out,
            "20 system ids, doors/switches/lifts/sprites mapped, "
            "save = .sbt + .sbw pair"))

    if want("scene"):
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
        tail = [l.strip() for l in p.stdout.strip().splitlines()[-3:]]
        ok = (p.returncode == 0 and len(tail) == 3
              and "1779 doors (1534 sliding, 200 linked), 3964 switches, 45 lifts" in tail[0]
              and "doors -> level objects: 1779/1779 valid (sector, object), 1779 distinct" in tail[1]
              and "switch ids in the level's entity table: 3281/3964" in tail[2])
        results.append(report("scene / doors, switches, lifts", ok,
                              "; ".join(tail) or "no output"))

    # --- optional heavy extraction ---------------------------------------
    if opts.full and want("extract"):
        results.extend(check_many([
            ("textures / PICS -> PNG",
             ["textures", G, os.path.join(out, "tex")], ["0 failed"]),
            ("sprites / sprites -> PNG",
             ["sprites", G, os.path.join(out, "spr")], ["0 failed"]),
            ("sounds / SOU0+SOU2 -> WAV, SOU1 raw",
             ["sounds", G, os.path.join(out, "snd")],
             ["13 SOU0 WAV, 983 SOU1 WAV, 3 SOU2 WAV, 0 raw dumps, 0 failed"]),
            ("frontend / BMP.DAT",
             ["frontend", G, os.path.join(out, "fe")], ["143 files"]),
        ]))
    if opts.full and want("film"):
        p = subprocess.run([sys.executable, os.path.join(HERE, "tools", "parity_film.py"),
                            "--quick"], capture_output=True, text=True, cwd=HERE)
        results.append(report("film / .sof/.fdx parity",
                              p.returncode == 0,
                              (p.stdout.strip().splitlines() or ["no output"])[-1]))

    passed = sum(results)
    scope = ("  [" + ",".join(sorted(ONLY)) + "]") if ONLY else ""
    hint = ""
    if not opts.full and not ONLY:
        hint = "  (use --full for extraction checks)"
    elif not opts.full:
        hint = "  (add --full for heavy checks in these categories)"
    print(f"\nregress: {passed}/{len(results)} checks passed in "
          f"{time.time() - T0:.1f}s" + scope + hint)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
