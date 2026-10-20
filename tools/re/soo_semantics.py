#!/usr/bin/env python3
"""Semantics checker for Gunmetal ``.soo`` model animations.

The format is plain text, but the *engine* meaning of its two header numbers is
not obvious from the files.  This tool models what ``gundx.exe`` actually does
(recovered from the loader at ``0x42b4e1`` and the player at ``0x433e00``, see
``docs/SOO.md``):

* ``count`` is authoritative: the loader reads exactly ``count`` frames from
  the text file and ignores anything after them;
* ``mode 0`` = play once and hold the last frame;
* ``mode 1`` = loop (the last frame morphs back into the first);
* durations are milliseconds;
* every frame name is a separate ``SOBS/<name>.sob`` pose; the engine morphs
  between poses, so all poses in one animation must share vertex/face counts;
* a missing pose or a topology mismatch makes the *whole* animation fail to
  load (``"Unable to load SOB or SOO %s"``).

Usage::

    python3 tools/re/soo_semantics.py                     # table, installed game
    python3 tools/re/soo_semantics.py --json
    python3 tools/re/soo_semantics.py --timeline BigGate
    python3 tools/re/soo_semantics.py --only Fire --frame-at 45
    python3 tools/re/soo_semantics.py --png /tmp/opencode/soo-sheets
    python3 tools/re/soo_semantics.py --strict            # exit 1 on failures

Only the Python standard library and this repository's pure-stdlib ``gm``
package are used; nothing is written to the game directory.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import soo as soomod  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
ENGINE_MAX_FRAMES = 32          # 0x42b5fa: count must be in 1..0x20
ENGINE_MAX_ANIMS = 512          # 0x42b551: global 0x5a45d0 < 0x200


# --------------------------------------------------------------------------
# discovery
# --------------------------------------------------------------------------

def find_soo(game: str) -> list[str]:
    d = os.path.join(game, "Sobanim")
    if not os.path.isdir(d):
        return []
    return sorted(p for p in glob.glob(os.path.join(d, "*"))
                  if p.lower().endswith(".soo"))


def sob_map(game: str) -> dict[str, str]:
    """lower-case stem -> actual ``SOBS/*.sob`` path (Windows is case-blind)."""
    d = os.path.join(game, "SOBS")
    out: dict[str, str] = {}
    if os.path.isdir(d):
        for p in glob.glob(os.path.join(d, "*")):
            if p.lower().endswith(".sob"):
                out[os.path.splitext(os.path.basename(p))[0].lower()] = p
    return out


def table_names(game: str) -> dict[str, list[str]]:
    """``lower(SOB_TABLE name)`` -> levels that reference it (all .sbt files)."""
    try:
        from gm import sbt as sbtmod
    except ImportError:  # pragma: no cover - gm always exists in this repo
        return {}
    refs: dict[str, list[str]] = {}
    for p in sorted(glob.glob(os.path.join(game, "*.sbt"))):
        try:
            world = sbtmod.parse_file(p)
        except (OSError, ValueError):
            continue
        level = os.path.splitext(os.path.basename(p))[0]
        for rec in world.records_of(20):
            for e in rec.payload.entries:
                refs.setdefault(e.name.lower(), []).append(level)
    return refs


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------

def _sob_dims(path: str) -> tuple[int, int] | None:
    from gm import sob as sobmod
    try:
        s = sobmod.parse(path)
    except (OSError, ValueError):
        return None
    return len(s.vertices), len(s.faces)


def analyse(path: str, game: str, sobs: dict[str, str],
            tables: dict[str, list[str]]) -> dict:
    name = os.path.basename(path)
    stem = os.path.splitext(name)[0]
    issues: list[str] = []
    warnings: list[str] = []
    try:
        a = soomod.parse_file(path)
    except (OSError, ValueError) as exc:
        return {"file": name, "path": path, "ok": False,
                "issues": ["parse: %s" % exc], "warnings": []}

    eff = a.engine_frames
    used = len(eff)
    dropped = a.dropped_frames
    total = a.engine_duration

    if a.mode not in (0, 1):
        issues.append("mode %d is not 0/1" % a.mode)
    if not 1 <= a.count <= ENGINE_MAX_FRAMES:
        issues.append("count %d outside engine range 1..%d"
                      % (a.count, ENGINE_MAX_FRAMES))
    if a.short:
        issues.append("short: count %d but only %d frame line(s)"
                      % (a.count, len(a.frames)))
    if dropped:
        warnings.append("dead: %d listed frame(s) after count are ignored"
                        % len(dropped))
    if total <= 0:
        issues.append("effective duration is not positive (%d ticks)" % total)
    for i, f in enumerate(eff):
        if f.duration <= 0:
            issues.append("frame %d (%s) has duration %d ticks"
                          % (i, f.name, f.duration))

    dims: list[tuple[int, int] | None] = []
    missing: list[str] = []
    for f in eff:
        sp = sobs.get(f.name.lower())
        if not sp:
            missing.append(f.name)
            dims.append(None)
            continue
        dims.append(_sob_dims(sp))
    if missing:
        issues.append("missing .sob: %s" % ", ".join(missing))

    first = next((d for d in dims if d), None)
    mismatched: list[str] = []
    if first:
        for i, (f, d) in enumerate(zip(eff, dims)):
            if d and d != first:
                mismatched.append("%d:%s %dv/%df != %dv/%df"
                                  % (i, f.name, d[0], d[1], first[0], first[1]))
    if mismatched:
        issues.append("topology mismatch: %s" % "; ".join(mismatched))

    refs = tables.get(stem.lower(), [])
    frame_refs: dict[str, list[str]] = {}
    for f in eff:
        r = tables.get(f.name.lower())
        if r:
            frame_refs[f.name] = sorted(set(r))

    return {
        "file": name,
        "path": path,
        "ok": not issues,
        "mode": a.mode,
        "loop": a.mode == 1,
        "count": a.count,
        "listed": len(a.frames),
        "used": used,
        "dropped": len(dropped),
        "clock_hz": soomod.ENGINE_TICK_HZ,
        "total_ticks": total,
        "total_ms": round(soomod.ticks_to_ms(total), 3),
        "listed_ticks": a.duration,
        "listed_ms": round(soomod.ticks_to_ms(a.duration), 3),
        "combined_header": a.combined,
        "frames": [{"name": f.name, "duration": f.duration,
                    "used": i < a.count,
                    "sob": bool(sobs.get(f.name.lower())),
                    "dims": (list(d) if d else None)} for i, (f, d)
                   in enumerate(zip(a.frames, dims + [None] * (len(a.frames) - len(dims))))],
        "missing": missing,
        "mismatched": mismatched,
        "table_name_refs": sorted(set(refs)),
        "table_frame_refs": frame_refs,
        "issues": issues,
        "warnings": warnings,
    }


# --------------------------------------------------------------------------
# textual report
# --------------------------------------------------------------------------

def print_table(rows: list[dict]) -> None:
    print("%-16s %4s %5s %5s %6s %9s  %s"
          % ("file", "mode", "count", "used", "drop", "ticks", "status"))
    for r in rows:
        if "count" not in r:
            print("%-16s %s" % (r["file"], r["issues"][0]))
            continue
        status = "ok" if r["ok"] else "FAIL"
        if r["ok"] and r["warnings"]:
            status = "warn"
        print("%-16s %4d %5d %5d %6d %8d  %s"
              % (r["file"], r["mode"], r["count"], r["used"],
                 r["dropped"], r["total_ticks"], status))
        for msg in r["issues"]:
            print("    ! %s" % msg)
        for msg in r["warnings"]:
            print("    ~ %s" % msg)
        if r["table_name_refs"]:
            print("    . used by SOB_TABLE in: %s" % ", ".join(r["table_name_refs"]))
        elif r["table_frame_refs"]:
            print("    . frame names used by SOB_TABLE: %s"
                  % ", ".join("%s (%s)" % (k, ",".join(v))
                              for k, v in r["table_frame_refs"].items()))
        else:
            print("    . not referenced by any SOB_TABLE")


def print_timeline(row: dict) -> None:
    a = soomod.parse_file(row["path"])
    print("%s: mode %d (%s), count %d, used %d, total %d ticks"
          % (row["file"], a.mode, "loop" if a.mode == 1 else "one-shot",
             a.count, len(a.engine_frames), a.engine_duration))
    for t in a.timeline():
        mark = "" if t.index < a.count else "  DEAD"
        print("  [%6d .. %6d) %-12s %5d ticks -> next %s%s"
              % (t.start, t.end, t.name, t.duration,
                 a.engine_frames[t.next_index].name, mark))
    if a.dropped_frames:
        print("  dropped (never read by the engine): %s"
              % ", ".join("%s %d ticks" % (f.name, f.duration)
                          for f in a.dropped_frames))
    if a.mode == 1 and a.engine_duration:
        print("  loop: t=%d ticks is t=0 again; last pose morphs to first"
              % a.engine_duration)


# --------------------------------------------------------------------------
# tiny software renderer for contact sheets (stdlib only)
# --------------------------------------------------------------------------

def _rgb888(c: int) -> tuple[int, int, int]:
    return (c >> 16) & 255, (c >> 8) & 255, c & 255


class Canvas:
    def __init__(self, w: int, h: int, color: int = 0x181818):
        self.w, self.h = w, h
        self.px = bytearray(_rgb888(color) * (w * h))

    def put(self, x: int, y: int, c: tuple[int, int, int] | int) -> None:
        if 0 <= x < self.w and 0 <= y < self.h:
            if isinstance(c, int):
                c = _rgb888(c)
            o = (y * self.w + x) * 3
            self.px[o:o + 3] = bytes(c)

    def line(self, x0: int, y0: int, x1: int, y1: int,
             c: tuple[int, int, int]) -> None:
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.put(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def rect(self, x0: int, y0: int, x1: int, y1: int,
             c: tuple[int, int, int]) -> None:
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.put(x, y, c)

    def tri(self, p0, p1, p2, c) -> None:
        xs = [p0[0], p1[0], p2[0]]
        ys = [p0[1], p1[1], p2[1]]
        minx, maxx = max(0, int(min(xs))), min(self.w - 1, int(max(xs)))
        miny, maxy = max(0, int(min(ys))), min(self.h - 1, int(max(ys)))
        d = ((p1[1] - p2[1]) * (p0[0] - p2[0])
             + (p2[0] - p1[0]) * (p0[1] - p2[1]))
        if d == 0:
            return
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                a = ((p1[1] - p2[1]) * (x - p2[0])
                     + (p2[0] - p1[0]) * (y - p2[1])) / d
                b = ((p2[1] - p0[1]) * (x - p2[0])
                     + (p0[0] - p2[0]) * (y - p2[1])) / d
                g = 1 - a - b
                if a >= 0 and b >= 0 and g >= 0:
                    self.put(x, y, c)


def _project(verts, az: float, el: float):
    ca, sa = math.cos(az), math.sin(az)
    ce, se = math.cos(el), math.sin(el)
    out = []
    for x, y, z in verts:
        rx = x * ca - y * sa
        ry = x * sa + y * ca
        sx = rx
        sy = -(z * ce - ry * se)
        depth = z * se + ry * ce
        out.append((sx, sy, depth))
    return out


def write_png(path: str, w: int, h: int, pixels: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    rows = [pixels[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    raw = b"".join(b"\x00" + r for r in rows)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def render_sheet(row: dict, sobs: dict[str, str], out_path: str,
                 tile: int = 128, cols: int | None = None) -> str:
    from gm import sob as sobmod
    a = soomod.parse_file(row["path"])
    frames = a.frames  # all listed, dead ones get a red border
    if not frames:
        raise ValueError("no frames")
    n = len(frames)
    cols = cols or min(8, n)
    rows = (n + cols - 1) // cols
    gap = 4
    W = cols * tile + (cols + 1) * gap
    H = rows * tile + (rows + 1) * gap
    cv = Canvas(W, H, 0x202020)

    parsed = {}
    allpts = []
    for f in frames:
        sp = sobs.get(f.name.lower())
        if not sp:
            parsed[f.name] = None
            continue
        try:
            s = sobmod.parse(sp)
        except (OSError, ValueError):
            parsed[f.name] = None
            continue
        pts = _project(s.vertices, math.radians(35), math.radians(22))
        parsed[f.name] = (s, pts)
        allpts += pts
    if not allpts:
        raise ValueError("no readable .sob frames")
    minx = min(p[0] for p in allpts)
    maxx = max(p[0] for p in allpts)
    miny = min(p[1] for p in allpts)
    maxy = max(p[1] for p in allpts)
    span = max(maxx - minx, maxy - miny, 1e-6)
    pad = tile * 0.10
    scale = (tile - 2 * pad) / span
    ox = (minx + maxx) / 2
    oy = (miny + maxy) / 2

    palette = [0x7fa8d8, 0xd8a25f, 0x82c07a, 0xc07fb0, 0xd0d070,
               0x70c0c0, 0xc07878, 0x9a8ad0]

    for idx, f in enumerate(frames):
        cx = gap + (idx % cols) * (tile + gap)
        cy = gap + (idx // cols) * (tile + gap)
        used = idx < a.count
        border = (0x50c050 if used else 0xd05050)
        cv.rect(cx, cy, cx + tile, cy + tile, (0x101010))
        cv.rect(cx, cy, cx + tile, cy + 2, border)
        cv.rect(cx, cy + tile - 2, cx + tile, cy + tile, border)
        cv.rect(cx, cy, cx + 2, cy + tile, border)
        cv.rect(cx + tile - 2, cy, cx + tile, cy + tile, border)
        item = parsed.get(f.name)
        if item is None:
            for t in range(8, tile - 8):
                cv.line(cx + t, cy + t, cx + tile - t, cy + tile - t,
                        (0xe06060))
                cv.line(cx + t, cy + tile - t, cx + tile - t, cy + t,
                        (0xe06060))
            continue
        s, pts = item
        proj = [((cx + tile / 2 + (p[0] - ox) * scale),
                 (cy + tile / 2 + (p[1] - oy) * scale), p[2]) for p in pts]
        tris = []
        for fi, face in enumerate(s.faces):
            idxs = face.indices
            for k in range(1, len(idxs) - 1):
                tri = (idxs[0], idxs[k], idxs[k + 1])
                depth = sum(proj[i][2] for i in tri) / 3.0
                tris.append((depth, fi, tri))
        tris.sort()
        for depth, fi, tri in tris:
            base = palette[fi % len(palette)]
            shade = max(0.35, min(1.0, 0.65 + depth / (span * 2)))
            c = (int(((base >> 16) & 255) * shade),
                 int(((base >> 8) & 255) * shade),
                 int((base & 255) * shade))
            cv.tri(proj[tri[0]], proj[tri[1]], proj[tri[2]], c)
            for k in range(3):
                p0, p1 = proj[tri[k]], proj[tri[(k + 1) % 3]]
                cv.line(int(p0[0]), int(p0[1]), int(p1[0]), int(p1[1]),
                        (max(0, c[0] - 45), max(0, c[1] - 45),
                         max(0, c[2] - 45)))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    write_png(out_path, W, H, bytes(cv.px))
    return out_path


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*",
                    help=".soo files or directories (default: <game>/Sobanim)")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="Gunmetal installation (default: %(default)s)")
    ap.add_argument("--json", action="store_true", help="machine-readable report")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when any animation would fail to load")
    ap.add_argument("--timeline", metavar="NAME",
                    help="print the effective timeline of one animation")
    ap.add_argument("--only", metavar="NAME",
                    help="restrict the report to one animation")
    ap.add_argument("--frame-at", type=int, metavar="MS",
                    help="with --only: print the pose at this offset")
    ap.add_argument("--png", metavar="DIR",
                    help="write a contact sheet per animation into DIR")
    ap.add_argument("--tile", type=int, default=128, help="tile size in pixels")
    ap.add_argument("--cols", type=int, default=None, help="columns per sheet")
    ap.add_argument("--no-sbt", action="store_true",
                    help="do not scan .sbt SOB_TABLE references")
    opts = ap.parse_args(argv)

    if opts.paths:
        paths: list[str] = []
        for p in opts.paths:
            if os.path.isdir(p):
                paths += sorted(q for q in glob.glob(os.path.join(p, "*"))
                                if q.lower().endswith(".soo"))
            else:
                paths.append(p)
    else:
        paths = find_soo(opts.game)
    if not paths:
        print("soo_semantics: no .soo files found", file=sys.stderr)
        return 2

    sobs = sob_map(opts.game)
    tables = {} if opts.no_sbt else table_names(opts.game)
    rows = [analyse(p, opts.game, sobs, tables) for p in paths]

    if opts.only:
        key = os.path.splitext(os.path.basename(opts.only))[0].lower()
        rows = [r for r in rows
                if os.path.splitext(r["file"])[0].lower() == key]
        if not rows:
            print("no such .soo: %s" % opts.only, file=sys.stderr)
            return 2

    if opts.frame_at is not None:
        if len(rows) != 1:
            print("--frame-at needs exactly one animation (use --only)",
                  file=sys.stderr)
            return 2
        a = soomod.parse_file(rows[0]["path"])
        try:
            pose = a.frame_at(opts.frame_at)
        except ValueError as exc:
            print("frame_at: %s" % exc, file=sys.stderr)
            return 1
        print("%s @ %d ms (tick %d): frame %d %s, %d/%d ticks, "
              "alpha=%.3f -> %s"
              % (rows[0]["file"], opts.frame_at, pose.phase, pose.index,
                 pose.name, pose.elapsed, pose.duration, pose.alpha,
                 pose.next_name))
        print("  phase=%d/%d ticks, %s"
              % (pose.phase, pose.total, "loop" if pose.loop else "one-shot"))
        return 0

    if opts.timeline:
        key = os.path.splitext(os.path.basename(opts.timeline))[0].lower()
        sel = [r for r in rows
               if os.path.splitext(r["file"])[0].lower() == key]
        if not sel:
            print("no such .soo: %s" % opts.timeline, file=sys.stderr)
            return 2
        print_timeline(sel[0])
        return 0

    if opts.png:
        done = 0
        for r in rows:
            if "count" not in r:
                continue
            out = os.path.join(opts.png, os.path.splitext(r["file"])[0] + ".png")
            try:
                render_sheet(r, sobs, out, tile=opts.tile, cols=opts.cols)
            except (OSError, ValueError) as exc:
                print("  FAIL %s: %s" % (r["file"], exc), file=sys.stderr)
                continue
            done += 1
            if not opts.json:
                print("  %s -> %s" % (r["file"], out))
        if not opts.json:
            print("png: %d contact sheet(s) in %s" % (done, opts.png))
        return 0

    bad = sum(1 for r in rows if not r["ok"])
    if opts.json:
        print(json.dumps({
            "game": opts.game,
            "summary": {"files": len(rows), "loadable": len(rows) - bad,
                        "failed": bad},
            "files": rows,
        }, indent=2, ensure_ascii=False))
    else:
        print_table(rows)
        print("soo: %d file(s), %d loadable, %d would fail to load"
              % (len(rows), len(rows) - bad, bad))
    return 1 if (opts.strict and bad) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
