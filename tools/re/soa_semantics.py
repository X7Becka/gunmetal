#!/usr/bin/env python3
"""Semantics checker for Gunmetal ``.soa`` sprite/texture animations.

The format is plain text but the engine meaning of its four header numbers is
not obvious from the files.  This tool models what ``gundx.exe`` actually does
(recovered from ``0x42b81b`` (loader), ``0x41de7d`` (frame selector),
``0x41df86`` (one-shot test) and ``0x44dc50`` (clock/film modes); see
``docs/SOA.md``):

* ``declared`` is authoritative: the loader reads exactly that many names with
  ``fscanf("%s")`` and ignores anything after them; fewer names than declared
  make the whole load fail;
* ``period`` is the **whole cycle in centiseconds**; the loader stores
  ``period * 91 / 100`` engine ticks (the engine clock is 91 Hz);
* ``mode 0`` = one shot (the last frame is held after one period, and effect
  sprites are freed);
* ``mode 1`` = loop (``t %= period_ticks``);
* ``mode 100/101/102`` = clock/film: the frame-0 texture is redrawn with the
  current local time / film frame, and the selector always returns frame 0;
* ``loop`` is stored but never read at run time (the loader also forces it to 0
  for ``spritani/`` files);
* every name is a sprite image: ``pics/<name>.sbm`` (kind 0), ``sprites/<name>.sbm``
  (kind 1) or ``flag/<name>.sbm`` (kind 4).  A missing image is a *deferred*
  failure (the entry is created and the .sbm is only read when drawn); a
  missing ``.soa`` instead falls back to a static sprite.

Usage::

    python3 tools/re/soa_semantics.py                 # table, installed game
    python3 tools/re/soa_semantics.py --json
    python3 tools/re/soa_semantics.py --check         # exit 1 on hard errors
    python3 tools/re/soa_semantics.py --only flames --frame-at 450
    python3 tools/re/soa_semantics.py --self-test     # no game needed

Only the Python standard library and this repository's pure-stdlib ``gm``
package are used; nothing is written to the game directory.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import bigfile  # noqa: E402
from gm import soa as soamod  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: directory -> (kind, sprite archive directory, fallback archive directory)
DIRS = {
    "anims": (0, "PICS", "sprites"),
    "spritani": (1, "sprites", "PICS"),
    "flaganim": (4, "FLAG", "PICS"),
    "sobanim": (0, "PICS", "sprites"),  # loader looks in anims/, see note
}


def _subdir(game: str, name: str) -> str:
    """``<game>/<name>`` with a case-insensitive match (Windows installs)."""
    path = os.path.join(game, name)
    if os.path.isdir(path):
        return path
    try:
        for entry in os.listdir(game):
            if entry.lower() == name.lower() and os.path.isdir(
                    os.path.join(game, entry)):
                return os.path.join(game, entry)
    except OSError:
        pass
    return path


# --------------------------------------------------------------------------
# discovery
# --------------------------------------------------------------------------

def find_soa(game: str) -> list[str]:
    """All ``*.soa`` files the shipped game has, in a stable order."""
    out: list[str] = []
    for sub in ("anims", "spritani", "flaganim", "Sobanim"):
        d = _subdir(game, sub)
        if not os.path.isdir(d):
            continue
        out += [p for p in glob.glob(os.path.join(d, "*"))
                if p.lower().endswith(".soa")]
    return sorted(out)


def dir_key(path: str, game: str) -> str:
    """Directory class of a ``.soa`` (``anims``/``spritani``/``Sobanim``)."""
    rel = os.path.relpath(os.path.dirname(os.path.abspath(path)),
                          os.path.abspath(game))
    parts = rel.replace(os.sep, "/").split("/")
    for part in reversed(parts):
        if part.lower() in DIRS:
            return part.lower()
    return "anims"


def kind_for(path: str, game: str) -> int:
    return DIRS[dir_key(path, game)][0]


def dirs_for(kind: int) -> tuple[str, str]:
    for key in ("anims", "spritani", "flaganim"):
        if DIRS[key][0] == kind:
            return DIRS[key][1], DIRS[key][2]
    return "PICS", "sprites"


def load_index(game: str, sub: str) -> set[str]:
    """Lower-case member names of ``<sub>/SBM.IDX`` (empty if missing)."""
    idx = os.path.join(_subdir(game, sub), "SBM.IDX")
    if not os.path.exists(idx):
        return set()
    try:
        return {e.name.lower() for e in bigfile.BigFile(idx)}
    except (ValueError, OSError):
        return set()


def load_files(game: str, sub: str, ext: str = ".sbm") -> set[str]:
    """Lower-case stems of loose files in ``<sub>/`` (e.g. FLAG/*.sbm)."""
    d = _subdir(game, sub)
    out: set[str] = set()
    if os.path.isdir(d):
        for p in glob.glob(os.path.join(d, "*")):
            stem, e = os.path.splitext(os.path.basename(p))
            if e.lower() == ext:
                out.add(stem.lower())
    return out


def image_tables(game: str) -> dict[str, set[str]]:
    """``PICS``/``sprites``/``FLAG`` name sets (archives + loose files)."""
    return {
        "PICS": load_index(game, "PICS") | load_files(game, "PICS"),
        "sprites": load_index(game, "sprites") | load_files(game, "sprites"),
        "FLAG": load_index(game, "FLAG") | load_files(game, "FLAG"),
    }


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------

def analyse(path: str, game: str, tables: dict[str, set[str]]) -> dict:
    rel = os.path.relpath(path, game).replace(os.sep, "/")
    rec: dict = {
        "file": rel,
        "name": os.path.splitext(os.path.basename(path))[0],
        "kind": kind_for(path, game),
        "dir": dir_key(path, game),
        "issues": [],
        "frames": [],
    }
    rec["kind_name"] = {"anims": "anims", "spritani": "spritani",
                        "flaganim": "flaganim", "sobanim": "Sobanim"}.get(
                            rec["dir"], rec["dir"])
    try:
        a = soamod.parse(open(path, "rb").read())
    except (ValueError, OSError) as exc:
        rec["error"] = str(exc)
        rec["issues"].append({"level": "error", "code": "parse", "detail": str(exc)})
        return rec

    primary_sub, alt_sub = dirs_for(rec["kind"])
    primary = tables.get(primary_sub, set())
    alt = tables.get(alt_sub, set())

    period_ticks = a.period_ticks
    authored_ms = a.period * 10
    rec.update({
        "mode": a.mode,
        "period": a.period,
        "period_ticks": period_ticks,
        "loop": a.loop,
        "declared": a.declared_frames,
        "listed": len(a.names),
        "special": a.special,
        "loadable": a.loadable,
        "authored_ms": authored_ms,
        "engine_ms": soamod.ms_at(period_ticks) if period_ticks > 0 else 0,
        "per_frame_ms": (authored_ms / a.declared_frames) if a.declared_frames else None,
        "primary": primary_sub,
        "alt": alt_sub,
    })

    if not a.names:
        rec["issues"].append({"level": "error", "code": "empty",
                              "detail": "no frame names"})
    if a.declared_frames < 1:
        rec["issues"].append({"level": "error", "code": "declared",
                              "detail": "declared %d < 1" % a.declared_frames})
    elif a.declared_frames > soamod.MAX_FRAMES:
        rec["issues"].append({"level": "error", "code": "declared",
                              "detail": "declared %d > %d (loader refuses)"
                                        % (a.declared_frames, soamod.MAX_FRAMES)})
    if a.short:
        rec["issues"].append({"level": "error", "code": "short",
                              "detail": "declared %d but only %d names (fscanf "
                                        "fails, whole file rejected)"
                                        % (a.declared_frames, len(a.names))})
    if a.truncated:
        rec["issues"].append({"level": "info", "code": "extra",
                              "detail": "%d extra name(s) after declared %d "
                                        "are never read"
                                        % (len(a.names) - a.declared_frames,
                                           a.declared_frames)})
    if a.mode not in (0, 1) and a.mode not in soamod.SPECIAL_MODES:
        rec["issues"].append({"level": "warn", "code": "mode",
                              "detail": "mode %d is not 0/1/100/101/102: "
                                        "engine returns frame 0" % a.mode})
    if a.mode in (0, 1) and period_ticks <= 0:
        rec["issues"].append({"level": "error", "code": "period",
                              "detail": "period %d -> %d ticks; engine divides "
                                        "by zero" % (a.period, period_ticks)})

    frames = a.engine_names()
    found_primary = found_alt = 0
    for i, name in enumerate(frames):
        low = name.lower()
        in_primary = low in primary
        in_alt = low in alt
        found_primary += in_primary
        found_alt += in_alt
        rec["frames"].append({"index": i, "name": name,
                              "in_primary": in_primary, "in_alt": in_alt})
        if not in_primary and not in_alt:
            rec["issues"].append({
                "level": "warn", "code": "image",
                "detail": "frame %d %r: no %s/%s.sbm nor %s/%s.sbm"
                          % (i, name, primary_sub, name, alt_sub, name)})
        elif not in_primary:
            rec["issues"].append({
                "level": "warn", "code": "image-alt",
                "detail": "frame %d %r: only in %s/ (loader asks %s/)"
                          % (i, name, alt_sub, primary_sub)})
    rec["found_primary"] = found_primary
    rec["found_alt"] = found_alt

    if not rec["loadable"]:
        rec["issues"].append({"level": "error", "code": "load",
                              "detail": "animation is not registered"})
    if rec["dir"] == "sobanim":
        rec["issues"].append({"level": "warn", "code": "misplaced",
                              "detail": "loader looks in anims/ for kind 0; a "
                                        "file in Sobanim/ is never found (static "
                                        "sprite fallback)"})
    if a.special:
        rec["issues"].append({"level": "info", "code": "special",
                              "detail": "mode %d: frame 0 texture is redrawn "
                                        "each tick (period ignored)" % a.mode})

    # Engine timeline (only meaningful for the two normal modes).
    rec["timeline"] = []
    if a.loadable and a.mode in (0, 1) and period_ticks > 0:
        for s in a.timeline():
            rec["timeline"].append({
                "index": s.index, "name": s.name,
                "start_tick": s.start_tick, "end_tick": s.end_tick,
                "start_ms": s.start_ms, "end_ms": s.end_ms,
                "duration_ticks": s.duration_ticks,
            })
    return rec


def summarize(records: list[dict], game: str) -> dict:
    """One summary dict for any subset of analysed records."""
    modes: dict[str, int] = {}
    kinds: dict[str, int] = {}
    for r in records:
        modes[str(r.get("mode"))] = modes.get(str(r.get("mode")), 0) + 1
        kinds[r["kind_name"]] = kinds.get(r["kind_name"], 0) + 1
    return {
        "game": game,
        "clock_hz": soamod.ENGINE_TICK_HZ,
        "tick_ms": 1000.0 / soamod.ENGINE_TICK_HZ,
        "files": len(records),
        "loadable": sum(1 for r in records if r.get("loadable")),
        "modes": modes,
        "kinds": kinds,
        "frames_listed": sum(r.get("listed", 0) for r in records),
        "frames_engine": sum(r.get("declared", 0) for r in records
                             if r.get("loadable")),
        "missing_images": sum(1 for r in records for f in r["frames"]
                              if not f["in_primary"] and not f["in_alt"]),
        "alt_images": sum(1 for r in records for f in r["frames"]
                          if not f["in_primary"] and f["in_alt"]),
        "issues": [i for r in records for i in r["issues"]],
        "errors": sum(1 for r in records
                      if any(i["level"] == "error" for i in r["issues"])),
        "warnings": sum(1 for r in records
                        if any(i["level"] == "warn" for i in r["issues"])),
        "records": records,
    }


def collect(game: str) -> dict:
    tables = image_tables(game)
    files = find_soa(game)
    records = [analyse(p, game, tables) for p in files]
    return summarize(records, game)


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------

def print_table(data: dict) -> None:
    print(f"game: {data['game']}")
    print(f"clock: {data['clock_hz']} Hz ({data['tick_ms']:.3f} ms/tick); "
          f"period is centiseconds -> ticks = period*91/100")
    print()
    head = ("file", "kind", "md", "per", "tick", "dur_ms", "dec/list",
            "ok", "alt", "note")
    print("%-28s %-8s %4s %4s %5s %7s %8s %4s %4s  %s" % head)
    print("-" * 108)
    for r in data["records"]:
        if "error" in r:
            print("%-28s %-8s  parse error: %s" % (r["file"], r["kind_name"],
                                                   r["error"]))
            continue
        note = []
        codes = [i["code"] for i in r["issues"]]
        if "short" in codes:
            note.append("LOADFAIL(short)")
        elif "declared" in codes:
            note.append("LOADFAIL(declared)")
        if "period" in codes:
            note.append("DIV0")
        if "extra" in codes:
            note.append("extra")
        if "image" in codes:
            note.append("missing")
        if "image-alt" in codes:
            note.append("other-dir")
        if r["special"]:
            note.append("special")
        if r["dir"] == "sobanim":
            note.append("misplaced")
        if r["mode"] == 1:
            note.append("loop")
        elif r["mode"] == 0:
            note.append("once")
        print("%-28s %-8s %4d %4d %5d %7d %5d/%-3d %4d %4d  %s"
              % (r["file"], r["kind_name"], r["mode"], r["period"],
                 r["period_ticks"], r["authored_ms"], r["declared"],
                 r["listed"], r["found_primary"], r["found_alt"],
                 ",".join(note)))
    print()
    print("files: %d (%s)" % (data["files"],
                              ", ".join("%s=%d" % kv for kv in
                                        sorted(data["kinds"].items()))))
    print("modes: %s" % ", ".join("%s=%d" % kv for kv in
                                  sorted(data["modes"].items())))
    print("loadable: %d/%d; names listed: %d, read by the engine: %d"
          % (data["loadable"], data["files"], data["frames_listed"],
             data["frames_engine"]))
    print("frame images: %d missing, %d only in the other kind's directory"
          % (data["missing_images"], data["alt_images"]))
    print("files with errors: %d, with warnings: %d"
          % (data["errors"], data["warnings"]))


def print_frame_at(records: list[dict], ms: int) -> int:
    bad = 0
    if ms < 0:
        print("negative time %d ms: the .soa engine has no clamp" % ms)
    else:
        print("time %d ms -> %d ticks (%d Hz)" % (ms, soamod.ticks_at(ms),
                                                  soamod.ENGINE_TICK_HZ))
    for r in records:
        if "error" in r:
            print("%-28s parse error" % r["file"])
            bad += 1
            continue
        try:
            a = soamod.parse(open(os.path.join(data_game, r["file"]),
                                  "rb").read())
            pose = a.frame_at(ms)
        except (ValueError, OSError) as exc:
            print("%-28s %s" % (r["file"], exc))
            bad += 1
            continue
        extra = " (special mode: frame 0 texture is redrawn)" if r["special"] else ""
        print("%-28s frame %2d/%d %-12s t=%d ticks%s"
              % (r["file"], pose.index, max(r.get("declared", 0), 1),
                 pose.name, pose.tick, extra))
    return 1 if bad else 0


# --------------------------------------------------------------------------
# self test (no game install required)
# --------------------------------------------------------------------------

def self_test() -> int:
    checks = 0

    def eq(got, want, what):
        nonlocal checks
        checks += 1
        if got != want:
            raise AssertionError("%s: got %r, want %r" % (what, got, want))

    def raises(fn, what):
        nonlocal checks
        checks += 1
        try:
            fn()
        except ValueError:
            return
        raise AssertionError("%s: expected ValueError" % what)

    # Tick conversion is a plain 91 Hz truncation.
    eq(soamod.ticks_at(0), 0, "ticks 0ms")
    eq(soamod.ticks_at(1000), 91, "ticks 1s")
    eq(soamod.ticks_at(11), 1, "ticks 11ms")
    eq(soamod.ticks_at(10), 0, "ticks 10ms")

    # period is centiseconds -> ticks = floor(period*91/100).
    a = soamod.from_names(1, 72, 1, ["a", "b"])
    eq(a.period_ticks, 65, "72cs ticks")
    eq(soamod.from_names(1, 55, 1, ["a"]).period_ticks, 50, "55cs ticks")
    eq(soamod.from_names(1, 1, 1, ["a"]).period_ticks, 0, "1cs ticks")

    # mode 1: wrap; whole cycle is `period`, each frame ~period/declared.
    b = soamod.from_names(1, 100, 1, ["a", "b", "c", "d"])   # 91 ticks
    eq([b.frame_at(ms).index for ms in (0, 260, 510, 760)],
       [0, 1, 2, 3], "loop quarters")
    eq(b.frame_at(1000).index, 0, "loop wrap")
    eq(b.frame_at(1254).index, 1, "loop second cycle")

    # mode 0: the last frame is held after one period.
    c = soamod.from_names(0, 100, 1, ["a", "b", "c", "d"])
    eq(c.frame_at(760).index, 3, "one-shot last")
    eq(c.frame_at(5000).index, 3, "one-shot hold")
    eq(c.frame_at(0).index, 0, "one-shot start")

    # Extra names are never read; a short file cannot be played.
    d = soamod.from_names(1, 100, 1, ["a", "b", "c", "d"])
    d.declared_frames = 2
    eq(d.engine_names(), ["a", "b"], "extra names dropped")
    eq(d.truncated, True, "truncated flag")
    raises(lambda: soamod.Soa(0, 100, 5, 1, ["a", "b"]).frame_at(0),
           "short file")

    # declared > 32 is refused by the loader.
    big = soamod.from_names(1, 100, 1, [str(i) for i in range(33)])
    eq(big.loadable, False, "declared > 32")
    raises(lambda: big.frame_at(0), "declared > 32 frame_at")

    # A zero-tick period would divide by zero inside the engine.
    z = soamod.from_names(1, 1, 1, ["a"])
    eq(z.period_ticks, 0, "period 1")
    raises(lambda: z.frame_at(0), "division by zero")

    # Special modes always return frame 0 and have an open-ended timeline.
    s = soamod.from_names(102, 1, 0, ["filmtext"])
    eq(s.frame_at(0).index, 0, "special frame")
    eq(s.frame_at(123456).name, "filmtext", "special name")
    eq(len(s.timeline()), 1, "special timeline")

    # Timeline boundaries: for P=18 ticks and 2 frames, the second starts at 9.
    t = soamod.from_names(0, 20, 1, ["a", "b"])               # 18 ticks
    seg = t.timeline()
    eq(seg[0].start_tick, 0, "timeline start")
    eq(seg[1].start_tick, 9, "timeline split")
    eq(seg[-1].end_tick, None, "one-shot open end")
    eq(t.frame_at(98).index, 0, "boundary before")
    eq(t.frame_at(99).index, 1, "boundary after")

    # Zero-length frames are possible when declared > period_ticks.
    zl = soamod.from_names(1, 10, 1, [str(i) for i in range(24)])  # 9 ticks
    eq(any(x.duration_ticks == 0 for x in zl.timeline()), True,
       "zero-length frame present")

    print("self-test: %d checks passed" % checks)
    return 0


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

data_game = DEFAULT_GAME


def main(argv=None) -> int:
    global data_game
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("paths", nargs="*", default=None,
                    help="game directory or .soa files (default: installed game)")
    ap.add_argument("--game", default=None,
                    help="game root (default: %s)" % DEFAULT_GAME)
    ap.add_argument("--json", action="store_true", help="machine-readable dump")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 on hard errors (load failures, div-by-zero)")
    ap.add_argument("--strict", action="store_true",
                    help="with --check, also fail on warnings")
    ap.add_argument("--only", default=None,
                    help="case-insensitive substring of the file path")
    ap.add_argument("--frame-at", type=int, default=None, metavar="MS",
                    help="show the frame at MS milliseconds after the start")
    ap.add_argument("--self-test", action="store_true",
                    help="run the model self-test (does not read the game)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.game:
        game = args.game
    elif args.paths and all(os.path.isfile(p) for p in args.paths):
        files = [os.path.abspath(p) for p in args.paths]
        game = os.path.commonpath([os.path.dirname(p) for p in files])
        if os.path.basename(game).lower() in DIRS:
            game = os.path.dirname(game)
    elif args.paths:
        game = args.paths[0]
        if os.path.isdir(game) and os.path.basename(
                game.rstrip("/\\")).lower() in DIRS:
            game = os.path.dirname(game.rstrip("/\\"))
    else:
        game = DEFAULT_GAME
    data_game = game
    if not os.path.isdir(game):
        print("game directory not found: %s" % game, file=sys.stderr)
        return 2

    data = collect(game)
    records = data["records"]
    if args.only:
        needle = args.only.lower()
        records = [r for r in records if needle in r["file"].lower()]
        if not records:
            print("no .soa matches %r" % args.only, file=sys.stderr)
            return 2

    if args.frame_at is not None:
        return print_frame_at(records, args.frame_at)

    if args.json:
        payload = (summarize(records, game)
                   if len(records) != len(data["records"]) else dict(data))
        json.dump(payload, sys.stdout, indent=2, ensure_ascii=False)
        sys.stdout.write("\n")
    else:
        if len(records) != len(data["records"]):
            print_table(summarize(records, game))
        else:
            print_table(data)

    if args.check or args.strict:
        bad = sum(1 for r in records if any(
            i["level"] == "error" or (args.strict and i["level"] == "warn")
            for i in r["issues"]))
        if bad:
            print("%d file(s) failed the check" % bad, file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
