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

    # --- level formats ----------------------------------------------------
    results.append(check("sbw / all levels", ["sbw", *sbw_files()], ["sbw: 36 parsed, 0 failed"]))
    results.append(check("sbt / all levels + resolve", ["sbt", G, "--resolve"],
                         ["sbt: 36 parsed, 0 failed"]))
    results.append(check("sbc / all levels",
                         ["sbc", *sorted(glob.glob(os.path.join(G, "*.sbc"))), out],
                         ["sbc: 36 files, 0 failed"]))

    # --- models / animations / audio -------------------------------------
    results.append(check("sob / models", ["sob", f"{G}/SOBS", out], ["sob: 672 converted, 0 failed"]))
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
