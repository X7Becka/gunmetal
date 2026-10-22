#!/usr/bin/env python3
"""Round-trip, binding and negative-case parity for ``.sof`` / ``.fdx`` films.

Usage::

    python3 tools/parity_film.py [--game DIR] [--json] [--dump DIR] [--quick]

The script only *reads* the installation (``--dump`` writes PNGs to a separate
directory and refuses to write inside the game tree).  It checks, in order:

1. **Round-trip.** Every ``.sof`` is parsed as 8 KB tiles and re-serialised;
   FNV-1a is printed for both sides.  Every ``.fdx`` / legacy ``.idx`` is
   parsed into ``(count, fps)`` records and re-emitted byte-exactly.
2. **Tile codec.** ``decode_tile(encode_tile(x)) == x`` for every shipped tile;
   the result is 128x64 and the transposed source is 64x128.
3. **Proof of the palette / layout.** All 30 ``Film/Kflm*.bmp`` dumped frames
   are byte-identical to frames 0..29 of ``Kelfilm.sof`` after the transpose,
   and every BMP carries the same palette (the film palette carrier).
4. **Binding.** ``<PREFIX><id>.SOF`` -> line ``id`` of ``<prefix>.fdx``; the
   expected families are AMMO/WEAPON/TANK/SHIELD/EQUIP/DEFAULT.  Scripted films
   are cross-checked against ``FILM_DEFINE`` (opcode 0xb3) in ``*.sbt``, where
   ``count`` must equal the tile count.
5. **Negative cases.** Truncated/partial tiles, oversized counters, bad index
   records, CRLF/LF variants, out-of-range frame lookups -- all must raise
   :class:`ValueError`/:class:`IndexError` (or be preserved verbatim) and never
   crash or hang.

Exit code 0 when every check passes.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from gm import bmp, sof  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

FDX_FILES = ("ammo.fdx", "default.fdx", "equip.fdx", "shield.fdx",
             "tank.fdx", "weapon.fdx")
LEGACY_IDX = ("ammo.idx",)

#: Families whose films are selected by the HUD wrapper (0x4958d0).
HUD_FAMILIES = ("weapon", "ammo", "tank", "shield", "equip", "default")


def crc32(data: bytes) -> int:
    """Fast checksum for the report (zlib's C loop; FNV here cost 17 s)."""
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def films(game: str) -> list[str]:
    folder = os.path.join(game, "Film")
    return sorted(os.path.join(folder, n) for n in os.listdir(folder)
                  if n.lower().endswith(".sof"))


def script_counts(game: str) -> dict[str, tuple[int, int, int]]:
    """``name -> (count, ms, loops)`` from FILM_DEFINE (0xb3) in ``*.sbt``.

    The signature is ``LPLS``; the operands are ``count, ms, flag, name``.
    """
    out: dict[str, tuple[int, int, int]] = {}
    try:
        from gm import sbt, sbtvm
    except Exception:                                    # pragma: no cover
        return out

    def walk(insns):
        for insn in insns:
            yield insn
            body = getattr(insn, "body", None)
            if body:
                yield from walk(body)

    for path in sorted(glob.glob(os.path.join(game, "*.sbt"))):
        try:
            parsed = sbt.parse_file(path)
            records = parsed.records_of(11)
        except (OSError, ValueError):
            continue
        for record in records:
            try:
                insns, _ = sbtvm.decode(record.payload.code)
            except (ValueError, IndexError):
                continue
            for insn in walk(insns):
                if insn.op == 179 and len(insn.args) >= 4:
                    count, ms, flag, name = insn.args[:4]
                    if isinstance(name, str):
                        out.setdefault(name.lower(), (count, ms, flag))
    return out


def bmp_frames(game: str) -> list[str]:
    folder = os.path.join(game, "Film")
    return sorted(os.path.join(folder, n) for n in os.listdir(folder)
                  if n.lower().startswith("kflm") and n.lower().endswith(".bmp"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_DIR", DEFAULT_GAME))
    ap.add_argument("--json", action="store_true", help="machine-readable rows")
    ap.add_argument("--dump", metavar="DIR", help="write sample frames as PNG")
    ap.add_argument("--quick", action="store_true",
                    help="skip the per-tile codec sweep (first/last/middle only)")
    opts = ap.parse_args(argv)

    ok = True
    rows: list[dict] = []

    def add(name: str, passed: bool, detail: str) -> None:
        nonlocal ok
        ok &= passed
        rows.append({"check": name, "ok": passed, "detail": detail})
        if not opts.json:
            print("%-4s %-52s %s" % ("ok" if passed else "FAIL", name, detail))

    folder = os.path.join(opts.game, "Film")
    if not os.path.isdir(folder):
        print("parity_film: no Film/ under %r" % opts.game, file=sys.stderr)
        return 2

    paths = films(opts.game)
    add("48 .sof films discovered", len(paths) == 48, "%d files" % len(paths))

    # ---------------------------------------------------------- 1. round-trip
    total_tiles = 0
    total_bytes = 0
    bad = []
    for path in paths:
        raw = open(path, "rb").read()
        try:
            film = sof.parse(raw, strict=True)
        except ValueError as exc:
            bad.append((os.path.basename(path), str(exc)))
            continue
        total_tiles += len(film)
        total_bytes += len(raw)
        rebuilt = film.to_bytes()
        if rebuilt != raw:
            bad.append((os.path.basename(path), "round-trip mismatch"))
        if not opts.json:
            print("     %-14s %4d tiles  %8d B  CRC %08x -> %08x"
                  % (os.path.basename(path), len(film), len(raw),
                     crc32(raw), crc32(rebuilt)))
    add("all .sof round-trip byte-exactly", not bad,
        "%d films, %d tiles, %d B%s"
        % (len(paths) - len(bad), total_tiles, total_bytes,
           "" if not bad else ", " + "; ".join("%s: %s" % b for b in bad)))

    # ----------------------------------------------------------- 2. tile codec
    codec_bad = []
    for path in paths:
        film = sof.parse_file(path)
        if opts.quick and len(film) > 3:
            probes = [0, len(film) // 2, len(film) - 1]
        else:
            probes = range(len(film))
        for i in probes:
            tile = film.tile(i)
            if len(tile) != sof.TILE_BYTES:
                codec_bad.append((os.path.basename(path), i, "tile size"))
                break
            pixels = sof.decode_tile(tile)
            if len(pixels) != sof.TILE_W * sof.TILE_H:
                codec_bad.append((os.path.basename(path), i, "frame size"))
                break
            if sof.encode_tile(pixels) != tile:
                codec_bad.append((os.path.basename(path), i, "inverse mismatch"))
                break
    add("tile codec is a bijection (all tiles)" if not opts.quick
        else "tile codec is a bijection (sample)", not codec_bad,
        "%d films checked" % len(paths) if not codec_bad
        else repr(codec_bad[:3]))
    add("decoded frame is 128x64 / stored tile is 64x128",
        sof.TILE_W == 128 and sof.TILE_H == 64 and sof.SRC_W == 64
        and sof.SRC_H == 128 and sof.TILE_BYTES == 0x2000,
        "%dx%d <- %dx%d" % (sof.TILE_W, sof.TILE_H, sof.SRC_W, sof.SRC_H))

    # ------------------------------------------------------- 3. fdx round-trip
    fdx_bad = []
    fdx_info = {}
    for name in FDX_FILES + LEGACY_IDX:
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            fdx_bad.append((name, "missing"))
            continue
        raw = open(path, "rb").read()
        try:
            fdx = sof.load_fdx(path)
        except ValueError as exc:
            fdx_bad.append((name, str(exc)))
            continue
        fdx_info[name] = [(e.count, e.fps) for e in fdx.entries]
        if fdx.to_bytes() != raw:
            fdx_bad.append((name, "round-trip mismatch"))
        if any(e.canonical != e.text for e in fdx.entries):
            fdx_bad.append((name, "non-canonical spacing"))
        if not opts.json:
            print("     %-12s %2d records %s"
                  % (name, len(fdx.entries),
                     ", ".join("%d@%d" % (c, f) for c, f in fdx_info[name][:4])
                     + (" ..." if len(fdx.entries) > 4 else "")))
    add("%d .fdx + %d legacy .idx round-trip"
        % (len(FDX_FILES), len(LEGACY_IDX)), not fdx_bad,
        "; ".join("%s: %s" % b for b in fdx_bad) or "canonical '%d, %d' CRLF")

    # ---------------------------------------------------------- 4. palette proof
    carriers = 0
    film_palette = None
    for path in glob.glob(os.path.join(folder, "*.bmp")) + \
            glob.glob(os.path.join(opts.game, "PICS", "*.bmp")):
        try:
            pal = bmp.parse(open(path, "rb").read()).palette
        except (OSError, ValueError):
            continue
        if film_palette is None:
            film_palette = pal
        elif pal == film_palette:
            carriers += 1
    try:
        shipped = sof.load_palette(opts.game)
    except (OSError, ValueError) as exc:
        shipped = None
        add("film palette carrier present", False, str(exc))
    if shipped is not None:
        add("film palette == dumped-film BMP palette", shipped == film_palette,
            "%d BMPs in Film/ + PICS/ share it, %d colours"
            % (carriers + 1, len(shipped) // 4))

    kel = os.path.join(folder, "Kelfilm.sof")
    frames = bmp_frames(opts.game)
    if os.path.exists(kel) and frames:
        film = sof.parse_file(kel)
        matched = 0
        pal_ok = True
        base_pal = None
        for path in frames:
            parsed = bmp.parse(open(path, "rb").read())
            base_pal = base_pal or parsed.palette
            pal_ok &= parsed.palette == base_pal
            index = int("".join(c for c in os.path.basename(path) if c.isdigit()))
            if index < len(film) and film.frame(index) == parsed.pixels:
                matched += 1
        add("Kflm*.bmp == Kelfilm.sof frames (transpose proof)",
            matched == len(frames) and pal_ok,
            "%d/%d BMPs, palette %s" %
            (matched, len(frames), "identical" if pal_ok else "DIFFERS"))
    else:
        add("Kflm*.bmp == Kelfilm.sof frames (transpose proof)", False,
            "Kelfilm.sof or Film/Kflm*.bmp missing")

    # --------------------------------------------------------------- 5. binding
    scripts = script_counts(opts.game)
    bind_rows = []
    bind_bad = []
    hud = 0
    for path in paths:
        info = sof.binding(path)
        prefix = info["prefix"]
        name = os.path.splitext(os.path.basename(path))[0].lower()
        ref = None
        if info["count"] is not None:
            hud += 1
            expected = info["tiles"] - info["count"]
            if expected not in (0, 1):
                bind_bad.append((info["file"], "count %s vs %d tiles"
                                 % (info["count"], info["tiles"])))
            elif prefix == "weapon" and info["id"] == 8 and expected != 0:
                bind_bad.append((info["file"], "weapon8 must have no padding"))
            elif expected == 1 and info["count"] != info["tiles"] - 1:
                bind_bad.append((info["file"], "padding arithmetic"))
            ref = "%s.fdx:%d" % (prefix, info["id"])
        elif name in scripts:
            count, ms, flag = scripts[name]
            if count != info["tiles"]:
                bind_bad.append((info["file"], "script count %d vs %d tiles"
                                 % (count, info["tiles"])))
            ref = "FILM_DEFINE(%d, ms=%d, %d)" % (count, ms, flag)
        else:
            ref = "unreferenced"
        info["ref"] = ref
        bind_rows.append(info)
        if not opts.json:
            print("     %-14s %-7s id=%-4s tiles=%-4d count=%-4s %s"
                  % (info["file"], prefix, info["id"], info["tiles"],
                     info["count"], ref))
    scripted = [r for r in bind_rows
                if r["ref"] and r["ref"].startswith("FILM_DEFINE")]
    script_bad = [b for b in bind_bad if "script count" in b[1]]
    add("HUD films resolve to their .fdx line", not bind_bad,
        "%d HUD films, %d scripted, %d unreferenced%s"
        % (hud, len(scripted),
           sum(1 for r in bind_rows if r["ref"] == "unreferenced"),
           "" if not bind_bad else "; " + repr(bind_bad[:4])))
    add("scripted FILM_DEFINE count == tiles", not script_bad,
        "%d scripted films cross-checked%s"
        % (len(scripted), "" if not script_bad else "; " + repr(script_bad[:3])))
    add("legacy ammo.idx differs from ammo.fdx line 0 (old exporter)",
        fdx_info.get("ammo.idx", [(None, None)])[0][0] !=
        fdx_info.get("ammo.fdx", [(None, None)])[0][0],
        "idx %s vs fdx %s" % (fdx_info.get("ammo.idx", [("?",)])[0],
                              fdx_info.get("ammo.fdx", [("?",)])[0]))

    # -------------------------------------------------------- 6. negative cases
    neg_bad = []
    saved_tiles = sof.MAX_TILES
    saved_lines = sof.MAX_FDX_LINES
    cases = [0]
    try:
        def _set_tiles(n):
            sof.MAX_TILES = n

        def _set_lines(n):
            sof.MAX_FDX_LINES = n

        def expect_value(label, fn):
            cases[0] += 1
            try:
                fn()
            except (ValueError, IndexError):
                return
            except Exception as exc:                     # noqa: BLE001
                neg_bad.append((label, "wrong exception %r" % (exc,)))
                return
            neg_bad.append((label, "accepted"))

        def expect_ok(label, fn):
            cases[0] += 1
            try:
                return fn()
            except Exception as exc:                     # noqa: BLE001
                neg_bad.append((label, "rejected: %r" % (exc,)))
                return None

        expect_value("empty frame lookup", lambda: sof.parse(b"").frame(0))
        expect_value("strict partial tile", lambda: sof.parse(b"\0" * 8191,
                                                             strict=True))
        expect_value("decode short tile", lambda: sof.decode_tile(b"\0" * 10))
        expect_value("encode short frame", lambda: sof.encode_tile(b"\0" * 10))
        expect_value("negative frame index", lambda: sof.frame(paths[0], -1))
        expect_value("frame past EOF", lambda: sof.frame(paths[0], 1 << 30))
        expect_value("fdx bad separator",
                     lambda: sof.parse_fdx_bytes(b"131 24\r\n"))
        expect_value("fdx non-numeric",
                     lambda: sof.parse_fdx_bytes(b"a, b\r\n"))
        expect_value("fdx BOM",
                     lambda: sof.parse_fdx_bytes(b"\xef\xbb\xbf131, 24\r\n"))
        expect_value("fdx zero fps",
                     lambda: sof.parse_fdx_bytes(b"131, 0\r\n"))
        expect_value("fdx negative fps",
                     lambda: sof.parse_fdx_bytes(b"131, -1\r\n"))
        expect_value("fdx huge count",
                     lambda: sof.parse_fdx_bytes(b"99999999999, 24\r\n"))
        expect_value("fdx lookup out of range",
                     lambda: sof.load_fdx(os.path.join(folder, "shield.fdx"))[1])
        expect_value("build_fdx rejects fps 0",
                     lambda: sof.build_fdx([(131, 0)]))

        partial = expect_ok("non-strict partial tile kept",
                            lambda: sof.parse(b"\0" * 8191))
        if partial is not None and partial.tail != b"\0" * 8191:
            neg_bad.append(("partial tail", "not preserved"))

        tail = expect_ok("one whole tile + tail",
                         lambda: sof.parse(b"\0" * 8193))
        if tail is not None and (len(tail) != 1 or tail.to_bytes() != b"\0" * 8193):
            neg_bad.append(("tail round-trip", "mismatch"))

        for raw, label in ((b"131, 24\r\n", "CRLF"),
                           (b"131, 24\n", "LF"),
                           (b"131, 24", "no EOL"),
                           (b"\r\n131, 24\r\n\r\n", "blank lines")):
            parsed = expect_ok("fdx %s" % label,
                               lambda raw=raw: sof.parse_fdx_bytes(raw))
            if parsed is not None and parsed.to_bytes() != raw:
                neg_bad.append(("fdx %s round-trip" % label, "mismatch"))

        built = expect_ok("canonical writer", lambda: sof.build_fdx([(131, 24)]))
        if built != b"131, 24\r\n":
            neg_bad.append(("canonical writer", repr(built)))

        # Limits last: they temporarily lower the module constants.
        expect_value("fdx line limit",
                     lambda: (_set_lines(2), sof.parse_fdx_bytes(
                         b"131, 24\r\n" * 3))[1])
        expect_value("sof tile limit",
                     lambda: (_set_tiles(2), sof.parse(b"\0" * 0x6000))[1])
    finally:
        sof.MAX_TILES = saved_tiles
        sof.MAX_FDX_LINES = saved_lines
    add("negative matrix rejects bad data, keeps valid layouts", not neg_bad,
        "%d cases" % cases[0] if not neg_bad else repr(neg_bad))

    # ---------------------------------------------------------------- 7. dump
    if opts.dump:
        target = os.path.abspath(opts.dump)
        game_abs = os.path.abspath(opts.game)
        if target == game_abs or target.startswith(game_abs + os.sep):
            print("parity_film: refusing to write inside the game tree",
                  file=sys.stderr)
            return 2
        os.makedirs(target, exist_ok=True)
        pal = sof.load_palette(opts.game)
        for name, index in (("Kelfilm.sof", 0), ("WEAPON1.SOF", 64),
                            ("AMMO4.SOF", 0), ("Default.sof", 144),
                            ("COMMS.SOF", 0)):
            path = os.path.join(folder, name)
            if os.path.exists(path):
                blob = sof.to_png(path, pal, index)
                out = os.path.join(target, "%s_%03d.png"
                                   % (os.path.splitext(name)[0].lower(), index))
                with open(out, "wb") as fh:
                    fh.write(blob)
        add("sample PNGs written", True, target)

    if opts.json:
        print(json.dumps({"ok": ok, "checks": rows,
                          "films": [{k: v for k, v in r.items() if k != "ref"}
                                    for r in bind_rows],
                          "fdx": fdx_info}, ensure_ascii=False, indent=1))
    else:
        passed = sum(1 for r in rows if r["ok"])
        print()
        print("film parity: %d/%d checks passed" % (passed, len(rows)))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
