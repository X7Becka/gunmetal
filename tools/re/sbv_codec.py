#!/usr/bin/env python3
"""Round-trip the Gunmetal ``.sbv`` pixel codec (research / verification tool).

``gm/sbv.py`` carries the container, the decoder and -- since this tool's
original run -- the *encoder*: it inverts the pyramidal transform and emits a
fresh entropy stream (classes 0 and 3 only).  This tool exercises that encoder
over the whole shipped corpus and exposes the individual frames.

What ``--check`` proves, per frame:

``decoded``        the frame block decodes without touching a guard/exception;
``pixel_exact``    ``decode(encode(decode(block)))`` reproduces the plane buffer
                   bit for bit (this is the codec round-trip);
``block_byte_exact`` the re-encoded block is byte-identical to the shipped one
                   (it is not: the original compressor's class/exponent choices
                   are not recoverable from a decoded frame -- reported honestly);
``container``      ``clip.to_bytes()`` re-serialises the whole clip byte for byte.

Usage::

    python3 tools/re/sbv_codec.py --check                    # whole Vid/*.sbv
    python3 tools/re/sbv_codec.py --check --jobs 16 --json
    python3 tools/re/sbv_codec.py Intro.sbv --png 42 --out frame.png
    python3 tools/re/sbv_codec.py --self-test                # no game data needed

The tool only reads the game directory.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbv as sbvmod  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# Worker state (populated once per process by _worker_init).
_WORK: dict = {}


def _worker_init(path: str) -> None:
    clip, blob = sbvmod.parse_file(path)
    _WORK["clip"] = clip
    _WORK["blob"] = blob


def _check_one(index: int) -> dict:
    clip = _WORK["clip"]
    try:
        frame = clip.frame(index)
        planes = sbvmod.decode_frame_planes(frame.data, clip.width, clip.height)
        lossy_before = sbvmod.LOSSY_PLANES
        block = sbvmod.encode_frame(planes, clip.width, clip.height)
        lossy = sbvmod.LOSSY_PLANES - lossy_before
        again = sbvmod.decode_frame_planes(block, clip.width, clip.height)
    except Exception as exc:  # noqa: BLE001 - reported, not hidden
        return {"index": index, "error": f"{type(exc).__name__}: {exc}"}
    return {
        "index": index,
        "orig_bytes": len(frame.data),
        "enc_bytes": len(block),
        "pixel_exact": again == planes,
        "block_byte_exact": block == frame.data,
        "lossy_planes": lossy,
        "crc_orig": zlib.crc32(planes) & 0xFFFFFFFF,
        "crc_round": zlib.crc32(again) & 0xFFFFFFFF,
    }


def _iter_frames(paths: list[str], jobs: int, limit: int | None):
    for path in paths:
        clip, _blob = sbvmod.parse_file(path)
        count = clip.frame_count if limit is None else min(limit, clip.frame_count)
        if jobs <= 1:
            _worker_init(path)
            for i in range(count):
                yield path, _check_one(i)
        else:
            with ProcessPoolExecutor(max_workers=jobs,
                                     initializer=_worker_init,
                                     initargs=(path,)) as pool:
                for i, res in zip(range(count), pool.map(_check_one, range(count))):
                    yield path, res


def cmd_check(paths: list[str], jobs: int, limit: int | None) -> dict:
    files = []
    totals = {"frames": 0, "decoded": 0, "pixel_exact": 0,
              "block_byte_exact": 0, "lossy_planes": 0,
              "orig_bytes": 0, "enc_bytes": 0}
    for path in paths:
        clip, blob = sbvmod.parse_file(path)
        start = time.time()
        n = clip.frame_count if limit is None else min(limit, clip.frame_count)
        stat = {
            "path": path,
            "width": clip.width,
            "height": clip.height,
            "frame_count": clip.frame_count,
            "checked": n,
            "bytes": len(blob),
            "data_start": clip.data_start,
            "tiles_eof": sbvmod.validate(clip),
            "container_byte_exact": clip.to_bytes() == blob,
            "decoded": 0,
            "pixel_exact": 0,
            "block_byte_exact": 0,
            "lossy_planes": 0,
            "orig_bytes": 0,
            "enc_bytes": 0,
            "errors": [],
        }
        prev_crc = None
        for _, res in _iter_frames([path], jobs, n):
            if "error" in res:
                stat["errors"].append({"index": res["index"], "error": res["error"]})
                continue
            stat["decoded"] += 1
            stat["pixel_exact"] += int(res["pixel_exact"])
            stat["block_byte_exact"] += int(res["block_byte_exact"])
            stat["lossy_planes"] += res["lossy_planes"]
            stat["orig_bytes"] += res["orig_bytes"]
            stat["enc_bytes"] += res["enc_bytes"]
            if prev_crc is not None and res["crc_orig"] != prev_crc:
                stat["frames_distinct"] = stat.get("frames_distinct", 0) + 1
            prev_crc = res["crc_orig"]
        stat["seconds"] = round(time.time() - start, 2)
        stat["ratio"] = (round(stat["enc_bytes"] / stat["orig_bytes"], 4)
                         if stat["orig_bytes"] else 0.0)
        totals["frames"] += stat["checked"]
        for key in ("decoded", "pixel_exact", "block_byte_exact", "lossy_planes",
                    "orig_bytes", "enc_bytes"):
            totals[key] += stat[key]
        files.append(stat)
    totals["ratio"] = (round(totals["enc_bytes"] / totals["orig_bytes"], 4)
                       if totals["orig_bytes"] else 0.0)
    totals["errors"] = sum(len(f["errors"]) for f in files)
    totals["ok"] = (all(f["decoded"] == f["checked"] for f in files)
                    and all(f["pixel_exact"] == f["checked"] for f in files)
                    and all(f["container_byte_exact"] for f in files)
                    and all(not f["errors"] for f in files))
    return {"tool": "sbv_codec", "mode": "check", "files": files, "totals": totals}


def cmd_png(path: str, index: int, out: str) -> dict:
    clip, _blob = sbvmod.parse_file(path)
    data = clip.to_png(index)
    with open(out, "wb") as f:
        f.write(data)
    rgb = clip.decode_frame(index)
    return {
        "path": path, "index": index, "out": out,
        "width": clip.width, "height": clip.height,
        "bytes": len(data), "crc_rgb": zlib.crc32(rgb) & 0xFFFFFFFF,
    }


# ---------------------------------------------------------------------------
# self-test: synthetic round-trip + every trust boundary that matters here
# ---------------------------------------------------------------------------

def _synth_block(width: int, height: int, nonzero: bool = True) -> bytes:
    """A self-consistent frame block built from a coarse grid + entropy blocks.

    The base grid stays inside 8 bits by construction, so re-encoding the
    decoded planes must be lossless.  ``nonzero`` exercises the class-3 path.
    """
    import struct as _struct

    bw = sbvmod._BitWriter()
    out = bytearray()
    for pl in range(3):
        pw, ph = (width, height) if pl == 0 else (width // 2, height // 2)
        lo, span = 1000, 200
        out += _struct.pack("<ii", lo, lo + span)
        nbase = (pw // 8) * (ph // 8)
        out += bytes((i * 37 + pl * 11) % 256 for i in range(nbase))
        bw.pred1 = bw.pred2 = 0
        for (sw, sh, factor) in sbvmod._pyramid_stages(pw, ph):
            bw.pred1 = bw.pred2 = 0
            size = (factor * factor * 3) >> 2
            for b in range((sw // factor) * (sh // factor)):
                vals = [0] * size
                if nonzero:
                    vals[b % size] = 64
                    vals[(b + 3) % size] = (-64) & 0xFFFF
                    vals[(b + 7) % size] = 128
                bw.block(vals)
        bw.flush()
        out += bw.out
        bw.out = bytearray()
    return bytes(out)


def self_test() -> int:
    import random
    import struct

    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, bool(ok), detail))

    def expect_raises(name: str, exc, fn) -> None:
        try:
            fn()
        except exc as e:  # noqa: PERF203
            check(name, True, type(e).__name__)
        except Exception as e:  # noqa: BLE001
            check(name, False, f"wrong exception {type(e).__name__}: {e}")
        else:
            check(name, False, "no exception")

    # --- encoder round-trip on synthetic, base-limited frames -------------
    for (w, h) in ((32, 32), (64, 64), (320, 160)):
        block = _synth_block(w, h)
        planes = sbvmod.decode_frame_planes(block, w, h)
        before = sbvmod.LOSSY_PLANES
        again_block = sbvmod.encode_frame(planes, w, h)
        lossy = sbvmod.LOSSY_PLANES - before
        again = sbvmod.decode_frame_planes(again_block, w, h)
        check(f"encode/decode {w}x{h} plane-exact", again == planes,
              f"synthetic {len(block)} -> re-encoded {len(again_block)} bytes")
        check(f"encode/decode {w}x{h} lossless base", lossy == 0, f"lossy={lossy}")

    # --- container writer round-trip (from re-encoded blocks) -------------
    w, h = 64, 64
    blocks = []
    for i in range(3):
        piece = sbvmod.decode_frame_planes(_synth_block(w, h), w, h)
        blocks.append((i * 66, sbvmod.encode_frame(piece, w, h)))
    blob = sbvmod.build(w, h, blocks)
    clip = sbvmod.parse(blob, "<self-test>")
    check("container parse/build round-trip", clip.to_bytes() == blob,
          f"{len(blob)} bytes")
    check("frames()/frame() from stored blob",
          len(clip.frames()) == 3 and clip.frame(1).index == 1)
    expect_raises("frame index out of range", IndexError, lambda: clip.frame(3))
    expect_raises("to_png palette size", ValueError,
                  lambda: clip.to_png(0, b"\x00" * 7))

    # --- negative: container trust boundaries -----------------------------
    bad = bytearray(blob)
    bad[:4] = b"XBv0"
    expect_raises("bad magic", ValueError, lambda: sbvmod.parse(bytes(bad)))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 4, sbvmod.MAX_WIDTH + 1)
    expect_raises("width over 320", ValueError, lambda: sbvmod.parse(bytes(bad)))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 8, 0)
    expect_raises("zero height", ValueError, lambda: sbvmod.parse(bytes(bad)))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 8, sbvmod.MAX_HEIGHT + 1)
    expect_raises("huge height", ValueError, lambda: sbvmod.parse(bytes(bad)))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 12, sbvmod.MAX_FRAMES + 1)
    expect_raises("huge frame count", ValueError, lambda: sbvmod.parse(bytes(bad)))
    expect_raises("truncated frame table", ValueError,
                  lambda: sbvmod.parse(blob[:16 + 8 * 3 - 4]))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 16 + 4, 4096)       # first end offset
    struct.pack_into("<I", bad, 16 + 8 + 4, 1024)   # second end offset goes backwards
    expect_raises("end offsets go backwards", ValueError,
                  lambda: sbvmod.parse(bytes(bad)))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 16 + 8 * 2 + 4, len(blob) * 4)
    expect_raises("frame runs past EOF", ValueError, lambda: sbvmod.parse(bytes(bad)))

    # --- negative: bitstream / geometry guards ----------------------------
    block = blocks[0][1]
    half = block[:len(block) // 2]
    expect_raises("truncated frame block", (ValueError, IndexError),
                  lambda: sbvmod.decode_frame_planes(half, w, h))
    expect_raises("frame geometry 8x32", ValueError,
                  lambda: sbvmod.decode_frame_planes(block, 8, 32))
    expect_raises("encoder geometry 320x24", ValueError,
                  lambda: sbvmod.encode_frame(b"\x00" * 4096, 320, 24))
    expect_raises("encoder short plane buffer", ValueError,
                  lambda: sbvmod.encode_frame(b"\x00" * 16, 32, 32))
    expect_raises("rgb buffer size", ValueError,
                  lambda: sbvmod.encode_frame_rgb(b"\x00" * 5, 32, 32))

    # --- negative: a full-range plane cannot be held by the 8-bit base ----
    random.seed(7)
    w, h = 32, 32
    cw, ch = w // 2, h // 2
    raw = bytearray(sbvmod.plane_buffer_size(w, h))
    vals = [random.randrange(65536) for _ in range(w * h)]
    struct.pack_into("<%dH" % (w * h), raw, 0, *vals)
    before = sbvmod.LOSSY_PLANES
    block = sbvmod.encode_frame(bytes(raw), w, h)
    lossy = sbvmod.LOSSY_PLANES - before
    again = sbvmod.decode_frame_planes(block, w, h)
    check("full-range plane flagged lossy", lossy >= 1, f"lossy={lossy}")
    check("full-range plane still decodes", len(again) == len(raw))
    check("full-range plane is not claimed exact", again != bytes(raw) or lossy > 0)

    # --- report -----------------------------------------------------------
    bad = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail else ""))
    print(f"{len(checks) - len(bad)}/{len(checks)} self-test checks passed")
    return 0 if not bad else 1


def corpus(game: str) -> list[str]:
    return sorted(glob.glob(os.path.join(game, "Vid", "*.sbv")))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("paths", nargs="*", help=".sbv clips (default: Vid/*.sbv)")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--check", action="store_true",
                    help="decode -> encode -> decode every frame of every clip")
    ap.add_argument("--png", type=int, metavar="INDEX", help="write frame INDEX as PNG")
    ap.add_argument("--out", metavar="FILE", help="PNG destination for --png")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic round-trip and negative/trust-boundary cases")
    ap.add_argument("--jobs", type=int, default=1,
                    help="worker processes for --check (default 1)")
    ap.add_argument("--limit", type=int, default=None,
                    help="only the first N frames of each clip")
    opts = ap.parse_args(argv)

    if opts.self_test:
        return self_test()

    paths = opts.paths or corpus(opts.game)
    if not paths:
        print(f"sbv_codec: no .sbv files under {opts.game}/Vid", file=sys.stderr)
        return 2

    if opts.png is not None:
        if len(paths) != 1:
            print("sbv_codec: --png needs exactly one clip", file=sys.stderr)
            return 2
        if not opts.out:
            print("sbv_codec: --png needs --out FILE", file=sys.stderr)
            return 2
        try:
            info = cmd_png(paths[0], opts.png, opts.out)
        except (OSError, ValueError, IndexError) as exc:
            print(f"sbv_codec: {exc}", file=sys.stderr)
            return 2
        if opts.json:
            print(json.dumps(info, indent=2))
        else:
            print(f"{info['path']}: frame {info['index']} {info['width']}x{info['height']} "
                  f"-> {info['out']} ({info['bytes']} bytes)")
        return 0

    if opts.check:
        if opts.jobs < 1:
            opts.jobs = os.cpu_count() or 1
        result = cmd_check(paths, opts.jobs, opts.limit)
        if opts.json:
            print(json.dumps(result, indent=2))
        else:
            for f in result["files"]:
                print(f"{os.path.basename(f['path'])}: {f['width']}x{f['height']} "
                      f"{f['frame_count']} frames, {f['bytes']} bytes, "
                      f"data_start={f['data_start']}")
                print(f"  decode {f['decoded']}/{f['checked']}, "
                      f"pixel-exact {f['pixel_exact']}/{f['checked']}, "
                      f"block-byte-exact {f['block_byte_exact']}/{f['checked']}, "
                      f"lossy planes {f['lossy_planes']}")
                print(f"  container rebuild byte-exact: {f['container_byte_exact']}, "
                      f"tiles to EOF: {f['tiles_eof']}, "
                      f"re-encoded {f['orig_bytes']} -> {f['enc_bytes']} bytes "
                      f"({f['ratio']}x), {f['seconds']}s")
                for err in f["errors"][:5]:
                    print(f"  ERROR frame {err['index']}: {err['error']}")
                if len(f["errors"]) > 5:
                    print(f"  ... {len(f['errors']) - 5} more error(s)")
            t = result["totals"]
            print(f"TOTAL: decode {t['decoded']}/{t['frames']}, "
                  f"pixel-exact {t['pixel_exact']}/{t['frames']}, "
                  f"block-byte-exact {t['block_byte_exact']}/{t['frames']}, "
                  f"lossy planes {t['lossy_planes']}, ratio {t['ratio']}x, "
                  f"ok={t['ok']}")
        return 0 if result["totals"]["ok"] else 1

    # default: header summary of every clip
    out = []
    for path in paths:
        clip, blob = sbvmod.parse_file(path)
        out.append({
            "path": path, "width": clip.width, "height": clip.height,
            "frames": clip.frame_count, "bytes": len(blob),
            "data_start": clip.data_start,
            "first_ts": clip.table[0][0] if clip.table else None,
            "last_ts": clip.table[-1][0] if clip.table else None,
            "tiles_eof": sbvmod.validate(clip),
            "block_min": min((e - p for p, (_t, e) in
                              zip([0] + [t[1] for t in clip.table],
                                  clip.table)), default=0),
            "block_max": max((e - p for p, (_t, e) in
                              zip([0] + [t[1] for t in clip.table],
                                  clip.table)), default=0),
        })
    if opts.json:
        print(json.dumps({"tool": "sbv_codec", "mode": "info", "files": out}, indent=2))
    else:
        for s in out:
            print(f"{os.path.basename(s['path'])}: {s['width']}x{s['height']} "
                  f"{s['frames']} frames, {s['bytes']} bytes, "
                  f"ts {s['first_ts']}..{s['last_ts']}, "
                  f"blocks {s['block_min']}..{s['block_max']}, tiles_eof={s['tiles_eof']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
