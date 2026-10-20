#!/usr/bin/env python3
"""Gunmetal asset toolkit CLI.

Examples
--------
    # list what is inside an archive
    gm.py list      "D:/games/Gunmetal 3dfx/PICS/SBM.IDX"

    # dump every texture as PNG (uses the game's shared palette)
    gm.py textures  "D:/games/Gunmetal 3dfx" out/textures

    # dump sprites as PNG with index 0 transparent
    gm.py sprites   "D:/games/Gunmetal 3dfx" out/sprites --transparent 0

    # dump sounds as WAV
    gm.py sounds    "D:/games/Gunmetal 3dfx" out/sounds

    # convert all .sob models to OBJ+MTL
    gm.py sob       "D:/games/Gunmetal 3dfx/SOBS" out/models

    # inspect the level files and cross-check the handle tables with the world
    gm.py sbt       "D:/games/Gunmetal 3dfx" --resolve
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gm import (  # noqa: E402
    sou1,
    sou2,
    bigfile,
    bmp,
    look,
    palette,
    png,
    render,
    sbc,
    sbm,
    sbt,
    sbtvm,
    sbv,
    sbw,
    sml,
    sob,
    soa,
    soe,
    sog,
    sou,
)


class _AtomicWriter:
    """Write next to *path* and rename on success, so a failed command never
    leaves a truncated half-file behind (and never touches the old one)."""

    def __init__(self, path: str):
        self.path = path
        directory = os.path.dirname(path) or "."
        fd, self.tmp = tempfile.mkstemp(dir=directory, prefix=".gm-tmp-")
        self.file = os.fdopen(fd, "wb")

    def __enter__(self):
        return self.file

    def __exit__(self, exc_type, _exc, _tb):
        self.file.close()
        if exc_type is None:
            try:
                os.chmod(self.tmp, 0o644)
            except OSError:
                pass
            os.replace(self.tmp, self.path)
        else:
            try:
                os.unlink(self.tmp)
            except OSError:
                pass
        return False


def _wopen(path: str, opts):
    """Open an output file, refusing to clobber it unless ``--force``.

    Trust boundary: every command that writes derived data goes through this
    helper, so a stray re-run cannot silently overwrite user files.  Writes are
    atomic: the data goes to a sibling temp file and is renamed into place only
    when the ``with`` block finishes without an exception.
    """
    if os.path.exists(path) and not getattr(opts, "force", False):
        raise FileExistsError(f"refusing to overwrite {path} (use --force)")
    return _AtomicWriter(path)


def _pal(opts) -> bytes:
    if opts.palette:
        return palette.load_from_bmp(opts.palette)
    return palette.installed(opts.game)


def cmd_list(opts) -> int:
    bf = bigfile.BigFile(opts.idx)
    print(f"{opts.idx}: {bf.count} entries, {bf.dat_size} bytes of data")
    for e in bf.entries:
        print(f"  {e.name:<12} off={e.offset:<10} size={e.size}")
    return 0


def cmd_extract(opts) -> int:
    bf = bigfile.BigFile(opts.idx)
    os.makedirs(opts.out, exist_ok=True)
    planned = []
    used: set[str] = set()
    for e in bf.entries:
        stem = bigfile.safe_name(e.name, f"unnamed_{e.offset:08x}")
        if stem.lower() in used:
            stem = f"{stem}_{e.offset:08x}"
        used.add(stem.lower())
        planned.append((os.path.join(opts.out, stem + ".bin"), e))
    if not opts.force:
        for path, _e in planned:
            if os.path.exists(path):
                raise FileExistsError(f"refusing to overwrite {path} (use --force)")
    for path, e in planned:
        with _AtomicWriter(path) as f:
            f.write(bf.read(e))
    print(f"wrote {len(planned)} blobs to {opts.out}")
    return 0


def cmd_pack(opts) -> int:
    blobs = []
    for f in sorted(os.listdir(opts.src)):
        p = os.path.join(opts.src, f)
        if not os.path.isfile(p):
            continue
        if opts.ext and not f.lower().endswith(opts.ext.lower()):
            continue
        blobs.append((os.path.splitext(f)[0], open(p, "rb").read()))
    if not blobs:
        print(f"pack: no files found in {opts.src}", file=sys.stderr)
        return 1
    try:
        n, size = bigfile.pack(blobs, opts.out, overwrite=opts.force)
    except ValueError as exc:
        print(f"pack: {exc}", file=sys.stderr)
        return 1
    print(f"pack: {n} entries, {size} bytes -> {opts.out}")
    return 0


def cmd_set(opts) -> int:
    blob = open(opts.file, "rb").read()
    try:
        dat = bigfile.replace_entry(opts.idx, opts.name, blob, opts.out, overwrite=opts.force)
    except ValueError as exc:
        print(f"set: {exc}", file=sys.stderr)
        return 1
    print(f"set: {opts.name} ({len(blob)} bytes) -> {opts.out} + {dat}")
    return 0


def cmd_sbm(opts) -> int:
    data = open(opts.image, "rb").read()
    low = opts.image.lower()
    if low.endswith(".png"):
        img = png.decode(data)
    elif low.endswith(".bmp"):
        from gm.png import Image
        bb = bmp.parse(data)
        img = Image(bb.width, bb.height, "P", 1, bb.pixels, bb.palette)
    else:
        print("sbm: input must be .png or .bmp", file=sys.stderr)
        return 1

    if opts.palette:
        pal = bmp.parse(open(opts.palette, "rb").read()).palette
    elif opts.game:
        pal = palette.installed(opts.game)
    elif img.palette:
        pal = img.palette
    else:
        print("sbm: pass --palette BMP or --game DIR (the image carries no palette)", file=sys.stderr)
        return 1

    prgb = [(pal[4 * i], pal[4 * i + 1], pal[4 * i + 2]) for i in range(len(pal) // 4)]
    cache: dict[tuple[int, int, int], int] = {}

    def nearest(rgb: tuple[int, int, int]) -> int:
        v = cache.get(rgb)
        if v is None:
            r, g, b = rgb
            best, bi = 1 << 30, 0
            for i, (pr, pg, pb) in enumerate(prgb):
                d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
                if d < best:
                    best, bi = d, i
            v = cache[rgb] = bi
        return v

    idx = bytearray()
    if img.mode == "P":
        ncol = len(img.palette) // 4
        remap = []
        for i in range(ncol):
            c = (img.palette[4 * i], img.palette[4 * i + 1], img.palette[4 * i + 2])
            # an identical palette keeps its indices (the shared palette has
            # duplicate colours, so a plain nearest-match would not round-trip)
            remap.append(i if i < len(prgb) and prgb[i] == c else nearest(c))
        for v in img.pixels:
            idx.append(remap[v] if v < len(remap) else 0)
    elif img.mode in ("L", "LA"):
        step = img.channels
        for i in range(0, len(img.pixels), step):
            v = img.pixels[i]
            idx.append(nearest((v, v, v)))
    else:  # RGB / RGBA
        step = img.channels
        for i in range(0, len(img.pixels), step):
            idx.append(nearest((img.pixels[i], img.pixels[i + 1], img.pixels[i + 2])))

    try:
        blob = sbm.encode(img.width, img.height, bytes(idx), levels=opts.mips,
                          flags=opts.flags, mip=opts.filter)
    except ValueError as exc:
        print(f"sbm: {exc}", file=sys.stderr)
        return 1
    with _wopen(opts.out, opts) as f:
        f.write(blob)
    print(f"sbm: {img.width}x{img.height}, {len(cache)} colours, {opts.mips} mip level(s), "
          f"{len(blob)} bytes -> {opts.out}")
    return 0


def cmd_textures(opts) -> int:
    pal = _pal(opts)
    src = opts.game
    idx = os.path.join(src, "PICS", "SBM.IDX")
    bf = bigfile.BigFile(idx)
    os.makedirs(opts.out, exist_ok=True)
    ok = bad = 0
    dat = open(bf.dat_path, "rb").read()
    for e in bf.entries:
        try:
            img = sbm.parse(e.data(dat))
        except ValueError:
            bad += 1
            continue
        png = img.to_png(pal, opts.transparent)
        with _wopen(os.path.join(opts.out, bigfile.safe_name(e.name) + ".png"), opts) as f:
            f.write(png)
        ok += 1
    print(f"textures: {ok} PNG written, {bad} failed -> {opts.out}")
    return 0 if bad == 0 else 1


def cmd_sprites(opts) -> int:
    pal = _pal(opts)
    bf = bigfile.BigFile(os.path.join(opts.game, "sprites", "SBM.IDX"))
    os.makedirs(opts.out, exist_ok=True)
    ok = bad = 0
    dat = open(bf.dat_path, "rb").read()
    for e in bf.entries:
        try:
            img = sbm.parse(e.data(dat))
        except ValueError:
            bad += 1
            continue
        with _wopen(os.path.join(opts.out, bigfile.safe_name(e.name) + ".png"), opts) as f:
            f.write(img.to_png(pal, opts.transparent))
        ok += 1
    print(f"sprites: {ok} PNG written, {bad} failed -> {opts.out}")
    return 0 if bad == 0 else 1


def cmd_sounds(opts) -> int:
    bf = bigfile.BigFile(os.path.join(opts.game, "sounds", "SOU.IDX"))
    os.makedirs(opts.out, exist_ok=True)
    dat = open(bf.dat_path, "rb").read()
    codecs: dict[str, int] = {}
    pcm = sou1_wav = music = coded = bad = 0
    for e in bf.entries:
        try:
            snd = sou.parse(e.data(dat))
        except ValueError:
            bad += 1
            continue
        codecs[snd.magic] = codecs.get(snd.magic, 0) + 1
        if snd.is_pcm:
            with _wopen(os.path.join(opts.out, bigfile.safe_name(e.name) + ".wav"), opts) as f:
                f.write(snd.to_wav())
            pcm += 1
        elif snd.magic == "SOU1":
            try:
                wav = sou1.to_wav(e.data(dat))
            except ValueError as exc:
                print(f"  {e.name}: {exc}", file=sys.stderr)
                bad += 1
                continue
            with _wopen(os.path.join(opts.out, bigfile.safe_name(e.name) + ".wav"), opts) as f:
                f.write(wav)
            sou1_wav += 1
        elif snd.magic == "SOU2":
            # Format 4 is fully decoded (gm/sou2.py); export real PCM.
            try:
                wav = sou2.to_wav(e.data(dat))
            except ValueError as exc:
                print(f"  {e.name}: {exc}", file=sys.stderr)
                bad += 1
                continue
            with _wopen(os.path.join(opts.out, bigfile.safe_name(e.name) + ".wav"), opts) as f:
                f.write(wav)
            music += 1
        else:
            # Honest raw dump: the payload is a driver-side codec, not PCM, so
            # no WAV is written (see gm/sou.py and docs/FORMATS.md).
            tag = "mono" if snd.channels == 1 else "stereo"
            name = (f"{bigfile.safe_name(e.name)}."
                    f"{snd.magic.lower()}.{snd.sample_rate}.{tag}.bin")
            with _wopen(os.path.join(opts.out, name), opts) as f:
                f.write(snd.payload)
            coded += 1
    print(f"sounds: {pcm} SOU0 WAV, {sou1_wav} SOU1 WAV, {music} SOU2 WAV, "
          f"{coded} raw dumps, {bad} failed")
    print(f"  containers: {codecs}")
    return 0 if bad == 0 else 1


def cmd_sou(opts) -> int:
    """Container stats/rebuild for sounds/SOU.IDX or MUSIC/SOU.IDX."""
    import json as _json
    from gm import sou as soumod

    bf = bigfile.BigFile(opts.idx)
    items = [(e.name, bf.read(e)) for e in bf.entries]
    report = soumod.stats(items)
    if opts.stats or not opts.rebuild:
        if opts.json:
            print(_json.dumps(report, indent=2, ensure_ascii=False))
        else:
            print("sou: %d entries (%d parsed, %d bad), %.1f s, %d bytes"
                  % (report["entries"], report["parsed"], report["bad"],
                     report["seconds"], report["bytes"]))
            print("  formats=%s rates=%s channels=%s"
                  % (report["formats"], report["rates"], report["channels"]))
            print("  loadable=%d anomalies=%s"
                  % (report["loadable"], report["anomalies"]))
    if not opts.rebuild:
        return 0 if report["bad"] == 0 else 1
    os.makedirs(opts.rebuild, exist_ok=True)
    ok = bad = 0
    for name, blob in items:
        try:
            data = soumod.parse(blob).rebuild()
            dst = os.path.join(opts.rebuild, bigfile.safe_name(name) + ".sou")
            with _wopen(dst, opts) as f:
                f.write(data)
            ok += 1
        except (ValueError, OSError) as exc:
            bad += 1
            print("  FAIL %s: %s" % (name, exc), file=sys.stderr)
    print("sou rebuild: %d written, %d failed -> %s" % (ok, bad, opts.rebuild))
    return 0 if bad == 0 else 1


def cmd_music(opts) -> int:
    """MUSIC/SOU.IDX -> one WAV per SOU2 entry (format-4 decoder)."""
    bf = bigfile.BigFile(opts.idx)
    os.makedirs(opts.out, exist_ok=True)
    dat = open(bf.dat_path, "rb").read()
    n = failed = skipped = 0
    seconds = 0.0
    for e in bf.entries:
        if opts.limit and n >= opts.limit:
            break
        blob = e.data(dat)
        try:
            rate, pcm = sou2.decode_sou2(blob, opts.volume)
        except ValueError as exc:
            if blob[:4] != sou2.MAGIC:
                skipped += 1
                continue
            print(f"  {e.name}: {exc}", file=sys.stderr)
            failed += 1
            continue
        name = bigfile.safe_name(e.name) + ".wav"
        seconds += (len(pcm) // 4) / rate
        with _wopen(os.path.join(opts.out, name), opts) as f:
            f.write(sou2.wav_from_pcm(rate, pcm))
        n += 1
    print(f"music: {n} WAV ({seconds:.1f} s), {skipped} non-SOU2 skipped, "
          f"{failed} failed -> {opts.out}")
    return 0 if failed == 0 else 1


def cmd_frontend(opts) -> int:
    bf = bigfile.BigFile(os.path.join(opts.game, "FRONTEND", "BMP.IDX"))
    os.makedirs(opts.out, exist_ok=True)
    n = 0
    dat = open(bf.dat_path, "rb").read()
    for e in bf.entries:
        blob = e.data(dat)
        ext = ".bmp" if blob[:2] == b"BM" else ".bin"
        with _wopen(os.path.join(opts.out, bigfile.safe_name(e.name) + ext), opts) as f:
            f.write(blob)
        n += 1
    print(f"frontend: {n} files -> {opts.out}")
    return 0


def cmd_sbc(opts) -> int:
    paths = list(_iter_with_rel(opts.paths, ".sbc"))
    paths.sort()
    if not paths:
        print("no .sbc files found", file=sys.stderr)
        return 1

    if not opts.check:
        os.makedirs(opts.out, exist_ok=True)
    ok = bad = 0
    for p, rel in paths:
        stem = os.path.splitext(rel)[0]
        try:
            blob = sbc.parse_file(p)
        except ValueError as exc:
            bad += 1
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            continue
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with _wopen(dst, opts) as f:
                f.write(blob.to_bytes())
        if not opts.check:
            out = os.path.join(opts.out, stem)
            os.makedirs(out, exist_ok=True)
            for i, rec in enumerate(blob.records):
                with _wopen(os.path.join(out, f"{i:04d}_{rec.width}x{rec.height}.png"), opts) as f:
                    f.write(rec.to_png())
            with _wopen(os.path.join(out, "_sheet.png"), opts) as f:
                f.write(_sbc_sheet(blob))
        ok += 1
    print(f"sbc: {ok} files, {bad} failed -> "
          f"{'(parsed only)' if opts.check else opts.out}")
    return 0 if bad == 0 else 1


def _sbc_sheet(blob, scale: int = 4, cols: int = 16, limit: int = 256) -> bytes:
    """One PNG with the first `limit` records laid out in a grid."""
    recs = blob.records[:limit]
    if not recs:
        return png.gray(1, 1, b"\x00")
    cell = max(r.width for r in recs) * scale
    cellh = max(r.height for r in recs) * scale
    rows = (len(recs) + cols - 1) // cols
    w, h = cols * cell, rows * cellh
    buf = bytearray(w * h)
    for i, r in enumerate(recs):
        cx, cy = (i % cols) * cell, (i // cols) * cellh
        for y in range(r.height * scale):
            src = (y // scale) * r.width
            dst = (cy + y) * w + cx
            row = r.pixels[src : src + r.width]
            buf[dst : dst + r.width * scale] = bytes(v for v in row for _ in range(scale))
    return png.gray(w, h, bytes(buf))


def _iter_with_rel(paths, ext):
    """Yield ``(path, relpath)`` for files matching *ext*.

    When a path is a directory, files are walked recursively and *relpath* is
    relative to that directory, so a rebuild preserves the sub-tree (the game
    has e.g. ``anims/``, ``spritani/`` and ``Sobanim/`` with colliding names).
    """
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in sorted(files):
                    if f.lower().endswith(ext):
                        fp = os.path.join(root, f)
                        yield fp, os.path.relpath(fp, p)
        else:
            yield p, os.path.basename(p)


def cmd_sml(opts) -> int:
    """Decode/encode the ``.sml`` UI markup and ``.sme`` briefings."""
    paths = list(_iter_markup(opts.paths))
    if not paths:
        print("no .sml/.sme files found", file=sys.stderr)
        return 1
    os.makedirs(opts.out, exist_ok=True)
    decoded = fixed = 0
    tags_seen: set = set()
    for p, rel in paths:
        with open(p, "rb") as f:
            blob = f.read()
        encoded = os.path.splitext(p)[1].lower() == ".sme"
        if encoded:
            decoded += 1
        if encoded:
            # decode_auto repairs a stray CR/LF only when the plain decode
            # yields unknown tag letters (see gm/sml.py)
            text, repaired = sml.decode_auto(blob)
            fixed += repaired
        else:
            text = blob.decode("latin1")
        plain = sml.plain(text)
        if opts.verbose:
            print(f"  {rel}: {len(blob)} B, "
                  f"{'decoded from .sme' if encoded else 'plain .sml'}, "
                  f"{len(plain.splitlines())} lines")
        for sub, ext, body in (("sml", ".sml", text), ("txt", ".txt", plain)):
            dst = os.path.join(opts.out, sub, rel + ext)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="latin1", newline="\n") as f:
                f.write(body)
        if opts.rebuild:
            # the .sme form of the (decoded) markup; for a shipped .sme this is
            # the exact original file (encode/decode are inverse)
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with _wopen(dst, opts) as f:
                f.write(sml.encode(text.encode("latin1")))
        tags_seen |= {t.name for t in sml.tags(text)}
    note = f", {fixed} repaired" if fixed else ""
    print(f"sml: {len(paths)} files ({decoded} decoded from .sme{note}), "
          f"{len(tags_seen)} distinct tags -> {opts.out}")
    return 0


def _iter_markup(paths):
    """Yield ``(path, relpath)`` for every ``.sml``/``.sme`` (either case)."""
    exts = (".sml", ".sme")
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in sorted(files):
                    if f.lower().endswith(exts):
                        fp = os.path.join(root, f)
                        yield fp, os.path.relpath(fp, p)
        else:
            yield p, os.path.basename(p)


def cmd_soa(opts) -> int:
    n = bad = 0
    report = []
    for p, rel in _iter_with_rel(opts.paths, ".soa"):
        try:
            a = soa.parse(open(p, "rb").read())
        except ValueError as exc:
            bad += 1
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            continue
        extra = "" if len(a.names) == a.frame_count else f" (+{len(a.names) - a.frame_count} extra)"
        try:
            intervals = a.timeline()
        except ValueError as exc:
            intervals = []
            timeline_error = str(exc)
        else:
            timeline_error = None
        record = {"file": os.path.basename(p), "path": p, "mode": a.mode,
                  "period": a.period, "period_ticks": a.period_ticks,
                  "loop": a.loop, "declared": a.frame_count,
                  "names": len(a.names), "loadable": a.loadable,
                  "clock_hz": soa.ENGINE_TICK_HZ,
                  "timeline": [{"index": x.index, "name": x.name,
                                "start_tick": x.start_tick,
                                "end_tick": x.end_tick,
                                "start_ms": x.start_ms, "end_ms": x.end_ms}
                               for x in intervals]}
        if timeline_error:
            record["timeline_error"] = timeline_error
        if opts.at is not None:
            try:
                pose = a.frame_at(opts.at)
                record["pose"] = {"at_ms": opts.at, "tick": pose.tick,
                                  "index": pose.index, "name": pose.name,
                                  "period_ticks": pose.period_ticks,
                                  "loop": pose.loop, "mode": pose.mode}
            except ValueError as exc:
                record["pose"] = {"at_ms": opts.at, "error": str(exc)}
        report.append(record)
        if not opts.json:
            print(f"{os.path.basename(p):16s} mode={a.mode} period={a.period} "
                  f"({a.period_ticks} ticks) loop={a.loop} "
                  f"frames={a.frame_count}{extra}")
            if opts.timeline and record.get("timeline_error"):
                print("    timeline: %s" % record["timeline_error"])
            if opts.timeline:
                for x in intervals:
                    end = "hold" if x.end_tick is None else str(x.end_tick)
                    print("      [%5d..%-5s) %d %-14s"
                          % (x.start_tick, end, x.index, x.name))
            if opts.at is not None and "pose" in record:
                pose = record["pose"]
                if "error" in pose:
                    print("    at %d ms: %s" % (opts.at, pose["error"]))
                else:
                    print("    at %d ms (tick %d): frame %d %s"
                          % (opts.at, pose["tick"], pose["index"], pose["name"]))
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with _wopen(dst, opts) as f:
                f.write(a.to_bytes())
        n += 1
    if opts.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print(f"soa: {n} parsed, {bad} failed")
    return 0 if bad == 0 else 1





def cmd_misc(opts) -> int:
    """Inspect the small formats: ``.sub``, ``.skk``/``.skb``, ``.sni``,
    ``.key``, ``.hi``."""
    from gm import config as gmconfig
    from gm import small
    paths = []
    for p in opts.paths:
        if os.path.isdir(p):
            for base, _dirs, files in os.walk(p):
                paths += [os.path.join(base, f) for f in sorted(files)
                          if os.path.splitext(f)[1].lower() in
                          (".sub", ".skk", ".skb", ".sni", ".key", ".hi",
                           ".cfg")]
        else:
            paths.append(p)
    n = bad = 0
    for path in paths:
        ext = os.path.splitext(path)[1].lower()
        try:
            raw = open(path, "rb").read()
            if ext == ".sub":
                obj = small.parse_sub(raw)
                summary = "%d entries%s" % (
                    len(obj.entries),
                    "" if obj.count is None else ", count %d" % obj.count)
                info = {"entries": [{"value": e.value, "left": e.left,
                                     "right": e.right} for e in obj.entries]}
            elif ext in (".skk", ".skb"):
                obj = small.parse_kill(raw)
                summary = "%d messages" % len(obj.messages)
                info = {"messages": obj.messages}
            elif ext == ".sni":
                obj = None
                text = small.parse_sni(raw)
                out = small.sni_to_bytes(text)
                summary = "%r" % text
                info = {"path": text}
            elif ext == ".key":
                obj = small.parse_key(raw)
                summary = "%r, %d records" % (obj.name, len(obj.entries))
                info = {"name": obj.name,
                        "codes": [c for c, _ in obj.entries]}
            elif ext == ".cfg":
                obj = gmconfig.parse(raw)
                summary = gmconfig.summary(obj)
                if isinstance(obj, gmconfig.OpaqueConfig):
                    info = {"header": obj.magic.decode("latin1").rstrip("\0"),
                            "version": obj.version, "opaque": True}
                else:
                    info = {"version": obj.version, "width": obj.width,
                            "height": obj.height,
                            "colour_depth": obj.colour_depth,
                            "key_profiles": [n.text for n in obj.key_names],
                            "opaque": False}
            elif ext == ".hi":
                obj = small.parse_hi(raw)
                summary = "%d records" % len(obj.records)
                info = {"records": [{"score": r["score"],
                                     "name": small.record_name(r)}
                                    for r in obj.records]}
            else:
                print("  SKIP %s: unsupported extension" % path)
                continue
        except (OSError, ValueError) as exc:
            bad += 1
            print("  FAIL %s: %s" % (path, exc), file=sys.stderr)
            continue
        blob = out if ext == ".sni" else obj.to_bytes()
        if opts.json:
            print(json.dumps({"file": os.path.basename(path), "type": ext,
                              "bytes": len(raw), **info},
                             ensure_ascii=False))
        else:
            print("%-22s %5d B  %s" % (os.path.basename(path), len(raw), summary))
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, os.path.basename(path))
            os.makedirs(opts.rebuild, exist_ok=True)
            with _wopen(dst, opts) as fh:
                fh.write(blob)
        n += 1
    if not opts.json:
        print("misc: %d parsed, %d failed" % (n, bad))
    return 0 if bad == 0 else 1


def cmd_film(opts) -> int:
    """Inspect ``.sof`` HUD films (8 KB tiles) and their ``.fdx`` indexes."""
    from gm import png, sof as sofmod
    paths = []
    index_paths = set()
    for p in opts.paths:
        if os.path.isdir(p):
            names = sorted(os.listdir(p))
            paths += [os.path.join(p, n) for n in names if n.lower().endswith(".sof")]
            index_paths.update(os.path.join(p, n) for n in names
                               if n.lower().endswith((".fdx", ".idx")))
        else:
            paths.append(p)
            if p.lower().endswith((".fdx", ".idx")):
                index_paths.add(p)
    palette = None
    if opts.palette:
        from gm import bmp
        palette = bmp.parse(open(opts.palette, "rb").read()).palette
    elif opts.apng:
        from gm import bmp
        folder = os.path.dirname(os.path.abspath(paths[0])) if paths else ""
        for cand in (folder, os.path.join(folder, "Film") if folder else ""):
            if not cand or not os.path.isdir(cand):
                continue
            name = next((n for n in os.listdir(cand)
                         if n.lower() == "kflm0000.bmp"), None)
            if name:
                palette = bmp.parse(open(os.path.join(cand, name), "rb").read()).palette
                break
        if palette is None:
            print("film: --apng needs --palette (Kflm0000.bmp not found)", file=sys.stderr)
            return 2
    n = bad = 0
    for path in paths:
        try:
            film = sofmod.parse_file(path)
            tiles, size = len(film), film.byte_size
            idx = sofmod.find_fdx(path)
            seqs = sofmod.parse_fdx(idx) if idx else []
            info = sofmod.binding(path)
        except (OSError, ValueError) as exc:
            bad += 1
            print("  FAIL %s: %s" % (path, exc), file=sys.stderr)
            continue
        if opts.json:
            print(json.dumps({"file": os.path.basename(path), "bytes": size,
                              "tiles": tiles, "fdx": idx,
                              "sequences": seqs, "binding": info}))
        else:
            print("%-14s %8d bytes, %d tiles (%dx%d)%s"
                  % (os.path.basename(path), size, tiles,
                     sofmod.TILE_W, sofmod.TILE_H,
                     "" if film.aligned else "  [not tile-aligned]"))
            if info.get("family"):
                print("    family=%s id=%s count=%s fps=%s extra=%s"
                      % (info["family"], info["id"], info["count"], info["fps"],
                         info["extra_tiles"]))
            if seqs:
                print("    %s: %d sequences, %s"
                      % (os.path.basename(idx), len(seqs),
                         ", ".join("%d@%d" % (s["count"], s["fps"]) for s in seqs[:6])
                         + (" ..." if len(seqs) > 6 else "")))
        if opts.png:
            try:
                data = film.frame(opts.frame)
            except ValueError as exc:
                print("  FAIL %s: %s" % (path, exc), file=sys.stderr)
                bad += 1
                continue
            out = opts.png
            if len(paths) > 1:
                stem = os.path.splitext(os.path.basename(path))[0]
                out = os.path.join(opts.png, "%s_%03d.png" % (stem, opts.frame))
                os.makedirs(opts.png, exist_ok=True)
            if palette:
                blob = png.indexed(data, sofmod.TILE_W, sofmod.TILE_H, palette)
            else:
                blob = png.gray(sofmod.TILE_W, sofmod.TILE_H, data)
            with _wopen(out, opts) as fh:
                fh.write(blob)
        if opts.apng:
            binding_count = info.get("count") or len(film)
            count = min(int(binding_count), len(film))
            if opts.limit_frames:
                count = min(count, opts.limit_frames)
            fps = opts.fps or int(info.get("fps") or 24)
            if count <= 0 or fps <= 0:
                print("  FAIL %s: no frames/fps for APNG" % path, file=sys.stderr)
                bad += 1
            else:
                out = opts.apng
                if len(paths) > 1 or os.path.isdir(out) or out.lower().endswith(os.sep):
                    os.makedirs(out, exist_ok=True)
                    out = os.path.join(out, os.path.splitext(os.path.basename(path))[0]
                                       + ".apng")
                with _wopen(out, opts) as fh:
                    fh.write(png.apng_indexed(film.frames()[:count],
                                              sofmod.TILE_W, sofmod.TILE_H,
                                              palette, fps))
                if not opts.json:
                    print("    apng: %d frames @ %d fps -> %s" % (count, fps, out))
        if opts.rebuild:
            os.makedirs(opts.rebuild, exist_ok=True)
            dst = os.path.join(opts.rebuild, os.path.basename(path))
            with _wopen(dst, opts) as fh:
                fh.write(film.to_bytes())
            if idx:
                index_paths.add(idx)
        n += 1
    if opts.rebuild:
        for idx in sorted(index_paths):
            try:
                fdx = sofmod.load_fdx(idx)
            except (OSError, ValueError) as exc:
                bad += 1
                print("  FAIL %s: %s" % (idx, exc), file=sys.stderr)
                continue
            os.makedirs(opts.rebuild, exist_ok=True)
            dst = os.path.join(opts.rebuild, os.path.basename(idx))
            if not os.path.exists(dst) or opts.force:
                with _wopen(dst, opts) as fh:
                    fh.write(fdx.to_bytes())
    if not opts.json:
        print("film: %d inspected, %d failed" % (n, bad))
    return 0 if bad == 0 else 1


def cmd_soo(opts) -> int:
    """Parse ``.soo`` flipbook animations (text: mode, count, name+time)."""
    from gm import soo as soomod
    sobs = set()
    if opts.game and os.path.isdir(os.path.join(opts.game, "SOBS")):
        sobs = {os.path.splitext(n)[0].lower()
                for n in os.listdir(os.path.join(opts.game, "SOBS"))}
    n = bad = missing = 0
    report = []
    for p, rel in _iter_with_rel(opts.paths, ".soo"):
        try:
            a = soomod.parse(open(p, "rb").read())
        except ValueError as exc:
            bad += 1
            print("  FAIL %s: %s" % (p, exc), file=sys.stderr)
            continue
        gap = len(a.frames) - a.count
        miss = [f.name for f in a.frames if sobs and f.name.lower() not in sobs]
        missing += len(miss)
        record = {"file": os.path.basename(p), "path": p, "mode": a.mode,
                  "loop": a.mode == 1, "count": a.count,
                  "frames": len(a.frames), "engine_frames": len(a.engine_frames),
                  "dropped": len(a.dropped_frames), "short": a.short,
                  "clock_hz": soomod.ENGINE_TICK_HZ,
                  "duration_ticks": a.duration,
                  "engine_duration_ticks": a.engine_duration,
                  "engine_duration_ms": round(a.engine_duration_ms, 3),
                  "timeline": [{"index": x.index, "name": x.name,
                                "duration_ticks": x.duration,
                                "start_tick": x.start, "end_tick": x.end,
                                "start_ms": round(x.start_ms, 3),
                                "end_ms": round(x.end_ms, 3),
                                "next": x.next_index} for x in a.timeline()],
                  "missing": miss}
        if opts.at is not None:
            try:
                pose = a.frame_at(opts.at)
                record["pose"] = {"at_ms": opts.at, "tick": pose.phase,
                                  "index": pose.index, "name": pose.name,
                                  "alpha": pose.alpha,
                                  "duration_ticks": pose.duration,
                                  "duration_ms": round(pose.duration_ms, 3),
                                  "next_index": pose.next_index,
                                  "next_name": pose.next_name}
            except ValueError as exc:
                record["pose"] = {"at": opts.at, "error": str(exc)}
        report.append(record)
        if not opts.json:
            extra = "" if gap == 0 else " (%+d vs declared)" % gap
            print("%-16s mode=%d frames=%d%s total=%d ticks (%.2f s)"
                  % (os.path.basename(p), a.mode, len(a.frames), extra,
                     a.duration, a.duration / soomod.ENGINE_TICK_HZ))
            if opts.timeline:
                print("    mode=%s count=%d engine=%d dropped=%d %s "
                      "total=%d ticks"
                      % ("loop" if a.mode == 1 else "one-shot", a.count,
                         len(a.engine_frames), len(a.dropped_frames),
                         "SHORT" if a.short else "ok", a.engine_duration))
                for x in a.timeline():
                    print("      [%5d..%-5d) %d %-12s -> %d"
                          % (x.start, x.end, x.index, x.name, x.next_index))
            if opts.at is not None:
                try:
                    pose = a.frame_at(opts.at)
                    print("    at %d ms (tick %d): frame %d %s alpha=%.3f -> %s"
                          % (opts.at, pose.phase, pose.index, pose.name,
                             pose.alpha, pose.next_name))
                except ValueError as exc:
                    print("    at %d ms: %s" % (opts.at, exc))
            if opts.verbose:
                for f in a.frames:
                    mark = "" if not sobs or f.name.lower() in sobs else "  (no .sob)"
                    print("    %-12s %6d ticks%s" % (f.name, f.duration, mark))
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            with _wopen(dst, opts) as f:
                f.write(a.to_bytes())
        n += 1
    if opts.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print("soo: %d parsed, %d failed, %d frame names without a .sob"
              % (n, bad, missing))
    return 0 if bad == 0 else 1


def cmd_look(opts) -> int:
    t = look.load(opts.file)
    st = t.stats()
    print(f"{os.path.basename(opts.file):14s} {st['dim']}^3 cube ({st['size']} bytes), "
          f"{st['distinct']} distinct values {st['min']}..{st['max']}")
    if opts.neons:
        n = look.parse_neons(opts.neons)
        vals = set(st["values"])
        print(f"  NEONS.PIN: {len(n)} indices, value-set match: {set(n) == vals}")
    if opts.png:
        pal = None
        if opts.palette:
            pal = bmp.parse(open(opts.palette, "rb").read()).palette
        w, h = t.to_png(opts.png, pal, opts.scale)
        print(f"  -> {opts.png} ({w}x{h})")
    return 0


def cmd_soe(opts) -> int:
    from gm import soe as soemod
    if opts.fields:
        rows = soemod.field_map()
        for fi in rows:
            print("0x%03x %-6s %-14s %-10s %s  [%s]"
                  % (fi.offset, fi.kind, fi.name, fi.status, fi.role,
                     ", ".join(fi.evidence)))
        counts = {}
        for fi in rows:
            counts[fi.status] = counts.get(fi.status, 0) + 1
        print("soe fields: %d (%s)"
              % (len(rows), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
        return 0
    if not opts.paths:
        print("soe: paths required unless --fields", file=sys.stderr)
        return 2
    n = bad = 0
    for p, rel in _iter_with_rel(opts.paths, ".soe"):
        try:
            tt = soe.parse_file(p)
        except (ValueError, OSError) as exc:
            bad += 1
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            continue
        n += 1
        extra = "" if tt.extra == 0 else f" (+{tt.extra} trailing tokens)"
        print(f"{os.path.basename(p):16s} version={tt.version} "
              f"collision={tt.get('collision_type')}/{tt.get('collision_shape')} "
              f"weapons={tt.get('weapon_count')} children={tt.get('children_count')}{extra}")
        if opts.verbose:
            for f in tt.fields:
                print(f"  {f.label:26s} {f.kind:3s} {f.value!r}")
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with _wopen(dst, opts) as f:
                f.write(tt.to_bytes())
    print(f"soe: {n} parsed, {bad} failed")
    return 0 if bad == 0 else 1


def cmd_sog(opts) -> int:
    files = list(_iter_with_rel([opts.path], ".sog"))
    if not files:
        print(f"no .sog files under {opts.path}", file=sys.stderr)
        return 1

    known = None
    if opts.music:
        bf = bigfile.BigFile(opts.music)
        known = {e.name.lower() for e in bf.entries}
        print(f"{opts.music}: {bf.count} music samples")

    bad = 0
    total = 0
    for p, rel in files:
        s = sog.parse(open(p, "rb").read())
        total += 1
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with _wopen(dst, opts) as f:
                f.write(s.to_bytes())
        missing = sum(1 for nm in s.names if nm.lower() not in known) if known else 0
        if opts.verbose or missing:
            print(f"{os.path.basename(p):12s} {s.track_count} samples"
                  + (f"  MISSING={missing}" if known is not None else ""))
        bad += 1 if missing else 0
    print(f"sog: {total} songs" + (f", {bad} with unresolved samples" if known else ""))
    return 0


def _parse_frame_spec(spec: str, count: int) -> list[int]:
    """'0', '1,5,9-12' or 'all' -> a sorted list of frame indices."""
    if spec in ("all", "*"):
        return list(range(count))
    out: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            lo, hi = int(a), int(b)
        else:
            lo = hi = int(part)
        if lo < 0:
            lo += count
        if hi < 0:
            hi += count
        out.update(range(lo, hi + 1))
    frames = sorted(out)
    for i in frames:
        if not 0 <= i < count:
            raise ValueError(f"frame {i} out of range (0..{count - 1})")
    return frames


def _sbv_load_raw(directory: str):
    """Read a raw-frame directory written by ``gm.py sbv FILE OUT``.

    Returns ``(width, height, [(timestamp_ms, block), ...])``.  The manifest was
    extended for this: ``frames.txt`` starts with a
    ``# sbv width=W height=H frames=N`` line, then ``index timestamp_ms offset
    size`` rows, next to the ``<index>.sbvf`` blocks.
    """
    txt_path = os.path.join(directory, "frames.txt")
    with open(txt_path, "r") as f:
        lines = f.read().splitlines()
    width = height = count = None
    stamps: dict[int, int] = {}
    for line in lines:
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            if "sbv width=" in line:
                for part in line.lstrip("# ").split():
                    if "=" in part:
                        k, v = part.split("=", 1)
                        if k == "width":
                            width = int(v)
                        elif k == "height":
                            height = int(v)
                        elif k == "frames":
                            count = int(v)
            continue
        parts = line.split()
        stamps[int(parts[0])] = int(parts[1])
    if width is None or height is None or count is None:
        raise ValueError(f"{txt_path}: no '# sbv width=.. height=.. frames=..' manifest line")
    frames = []
    for i in range(count):
        if i not in stamps:
            raise ValueError(f"{txt_path}: no timestamp row for frame {i}")
        with open(os.path.join(directory, f"{i:05d}.sbvf"), "rb") as f:
            frames.append((stamps[i], f.read()))
    return width, height, frames


def _cmd_sbv_build(opts) -> int:
    """Encode a clip out of a raw-frame directory (the OUT of a parse run)."""
    if not opts.out:
        print("sbv: building from a raw directory needs the target as OUT "
              "(FILE=directory, OUT=<clip>.sbv)", file=sys.stderr)
        return 1
    try:
        width, height, frames = _sbv_load_raw(opts.file)
    except (OSError, ValueError) as exc:
        print(f"sbv: {exc}", file=sys.stderr)
        return 1
    blob = sbv.build(width, height, frames)
    dst = opts.out
    if os.path.isdir(dst) or dst.endswith(("/", os.sep)):
        os.makedirs(dst, exist_ok=True)
        dst = os.path.join(dst, os.path.basename(os.path.normpath(opts.file)) + ".sbv")
    elif os.path.dirname(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
    with _wopen(dst, opts) as f:
        f.write(blob)
    print(f"sbv: {width}x{height}, {len(frames)} frames, {len(blob)} bytes -> {dst}")
    return 0


def cmd_sbv(opts) -> int:
    if os.path.isdir(opts.file):
        return _cmd_sbv_build(opts)
    clip, blob = sbv.parse_file(opts.file)
    if not sbv.validate(clip):
        print("warning: frame offsets do not tile the file", file=sys.stderr)
    print(f"{opts.file}: {clip.width}x{clip.height}, {clip.frame_count} frames, "
          f"data_start={clip.data_start}, size={clip.file_size}")
    if opts.list_only:
        for i, (ts, end) in enumerate(clip.table):
            prev = clip.table[i - 1][1] if i else 0
            print(f"  {i:4d} t={ts:6d}ms off={clip.data_start + prev:<9d} size={end - prev}")
        return 0

    if opts.verify:
        frames = _parse_frame_spec(opts.frames, clip.frame_count)
        exact = 0
        for i in frames:
            planes = sbv.decode_frame_planes(clip.frame(blob, i).data,
                                             clip.width, clip.height)
            again = sbv.decode_frame_planes(
                sbv.encode_frame(planes, clip.width, clip.height),
                clip.width, clip.height)
            if again == planes:
                exact += 1
            else:
                print(f"  frame {i}: plane mismatch", file=sys.stderr)
        print(f"sbv: pixel round-trip {exact}/{len(frames)} frames")
        if exact != len(frames):
            return 1

    if opts.png:
        frames = _parse_frame_spec(opts.frames, clip.frame_count)
        os.makedirs(opts.png, exist_ok=True)
        W, H = clip.width * opts.scale, clip.height * opts.scale
        for i in frames:
            rgb = sbv.decode_frame_rgb(clip.frame(blob, i).data, clip.width, clip.height)
            if opts.scale != 1:
                rgb = _scale_rgb(rgb, clip.width, clip.height, opts.scale)
            out = os.path.join(opts.png, f"{i:05d}.png")
            with _wopen(out, opts) as f:
                f.write(png.rgb(W, H, rgb))
        print(f"sbv: {len(frames)} frame(s) decoded -> {opts.png}")
        if sbv.FAMILY_BLOCKS:
            print(f"warning: {sbv.FAMILY_BLOCKS} block(s) used an approximated "
                  f"class >= 2 code path", file=sys.stderr)

    if opts.out and not opts.no_raw:
        os.makedirs(opts.out, exist_ok=True)
        with open(os.path.join(opts.out, "frames.txt"), "w") as f:
            f.write(f"# sbv width={clip.width} height={clip.height} "
                    f"frames={clip.frame_count}\n")
            f.write("# index timestamp_ms offset size\n")
            prev = 0
            for i, (ts, end) in enumerate(clip.table):
                f.write(f"{i} {ts} {clip.data_start + prev} {end - prev}\n")
                prev = end
        for fr in clip.frames(blob):
            with _wopen(os.path.join(opts.out, f"{fr.index:05d}.sbvf"), opts) as f:
                f.write(fr.data)
        print(f"sbv: {clip.frame_count} frames -> {opts.out} (raw blocks + frames.txt)")

    if opts.rebuild:
        os.makedirs(opts.rebuild, exist_ok=True)
        dst = os.path.join(opts.rebuild, os.path.basename(opts.file))
        out = clip.to_bytes(blob)
        with _wopen(dst, opts) as f:
            f.write(out)
        print(f"sbv: re-serialised -> {dst}"
              + ("" if out == blob else "  (differs from the source)"))
    return 0


def _scale_rgb(rgb: bytes, width: int, height: int, scale: int) -> bytes:
    """Nearest-neighbour integer upscale."""
    out = bytearray(width * scale * height * scale * 3)
    stride = width * scale * 3
    for y in range(height):
        row = rgb[y * width * 3:(y + 1) * width * 3]
        up = bytearray()
        for x in range(width):
            up += row[x * 3:x * 3 + 3] * scale
        for k in range(scale):
            out[(y * scale + k) * stride:(y * scale + k) * stride + stride] = up
    return bytes(out)


def cmd_sbw(opts) -> int:
    ok = bad = 0
    for p in opts.files:
        try:
            w = sbw.parse_file(p)
        except ValueError as exc:
            bad += 1
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            continue
        ok += 1
        size = os.path.getsize(p)
        nthr = f"{w.data_end}/{size}" if w.data_end == size else f"{w.data_end}/{size} MISMATCH"
        print(f"{os.path.basename(p):14s} {len(w.tex)} textures, {len(w.effects)} effects, "
              f"{len(w.sectors)} sectors, {w.n_objects} objects, {w.n_polys} polys "
              f"(parse {nthr})")
        if opts.verbose:
            for t in w.tex[:10]:
                print(f"  tex  {t.name:12s} flag={t.flag} value={t.value}")
            for e in w.effects[:10]:
                print(f"  fx   {e.name:12s} kind={e.kind} start={e.start} ids={e.ids}")
            for si, s in enumerate(w.sectors[:3]):
                print(f"  sector[{si}] kind={s.kind} sub={s.sub} objects={len(s.objects)}")
                for oj, o in enumerate(s.objects[:3]):
                    print(f"    obj[{oj}] id={o.id} verts={o.n_verts} polys={len(o.polys)} "
                          f"origin={tuple(round(c, 3) for c in o.points[0])}")
        if opts.obj:
            os.makedirs(opts.obj, exist_ok=True)
            out = os.path.join(opts.obj, os.path.splitext(os.path.basename(p))[0] + ".obj")
            n = sbw.to_obj(w, out, transform=opts.transform, tex=opts.verbose)
            print(f"  -> {out} ({n} polygons)")
        if opts.rebuild:
            os.makedirs(opts.rebuild, exist_ok=True)
            out = os.path.join(opts.rebuild, os.path.basename(p))
            with _wopen(out, opts) as f:
                f.write(w.to_bytes())
    print(f"sbw: {ok} parsed, {bad} failed")
    return 0 if bad == 0 else 1


def _sbt_iter(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                # SAVEGAME holds save containers (.sbt in name only), not levels
                dirs[:] = [d for d in dirs if d.lower() != "savegame"]
                for f in sorted(files):
                    if f.lower().endswith(".sbt"):
                        yield os.path.join(root, f)
        else:
            yield p


def _sbt_json(obj):
    """JSON-friendly view of an .sbt (raw byte blobs collapse to their size)."""
    if isinstance(obj, bytes):
        return {"bytes": len(obj)}
    if dataclasses.is_dataclass(obj):
        return {
            f.name: _sbt_json(getattr(obj, f.name))
            for f in dataclasses.fields(obj)
            if not (f.name in ("raw",) and not getattr(obj, f.name))
        }
    if isinstance(obj, (list, tuple)):
        return [_sbt_json(x) for x in obj]
    return obj


def _sbt_resolve(s, sbw_path):
    """Check every handle of *s* against the geometry in *sbw_path*.

    The handle tables store the engine's canonical ``(sector, object, poly)``
    address, so every row must point at an existing polygon of the sibling
    ``.sbw``.  Returns ``(summary, problems)`` where *summary* maps a record type
    name to ``(total, in_range)`` and *problems* lists individual bad rows.
    """
    w = sbw.parse_file(sbw_path)
    summary: dict[str, tuple[int, int]] = {}
    problems: list[str] = []

    def object_ok(sec, obj) -> bool:
        return sec < len(w.sectors) and obj < len(w.sectors[sec].objects)

    def poly_ok(sec, obj, pol) -> bool:
        return object_ok(sec, obj) and pol < len(w.sectors[sec].objects[obj].polys)

    for rec in s.records:
        p = rec.payload
        if isinstance(p, sbt.PolyHandles):
            good = 0
            for i in range(len(p)):
                if poly_ok(p.sectors[i], p.objects[i], p.polys[i]):
                    good += 1
                else:
                    problems.append(
                        f"POLY_HANDLE #{i}: tag={p.tags[i]} "
                        f"{p.sectors[i]}/{p.objects[i]}/{p.polys[i]}")
            summary["POLY_HANDLE"] = (len(p), good)
        elif isinstance(p, sbt.ObjectHandles):
            good = sum(1 for i in range(len(p)) if object_ok(p.sectors[i], p.objects[i]))
            summary["OBJECT_HANDLE"] = (len(p), good)
        elif isinstance(p, sbt.SectorHandles):
            good = sum(1 for i in range(len(p)) if p.sectors[i] < len(w.sectors))
            summary["SECTOR_HANDLE"] = (len(p), good)
    return summary, problems



def cmd_save(opts) -> int:
    """Inspect a save (doors, switches, player, ammo) or diff two saves."""
    here = os.path.dirname(os.path.abspath(__file__))
    re_dir = os.path.join(here, "tools", "re")
    if re_dir not in sys.path:
        sys.path.insert(0, re_dir)
    import savefile
    import savediff

    if opts.rebuild:
        import save_rebuild
        try:
            save = save_rebuild.load(opts.path)
            data = save.to_bytes()
        except (OSError, ValueError) as exc:
            print("save: %s" % exc, file=sys.stderr)
            return 1
        with open(opts.path, "rb") as fh:
            original = fh.read()
        if opts.out:
            with _wopen(opts.out, opts) as fh:
                fh.write(data)
            print("save rebuild: %s -> %s (%d bytes)"
                  % (os.path.basename(opts.path), opts.out, len(data)))
        else:
            print("save rebuild: %s, %d bytes, %s"
                  % (os.path.basename(opts.path), len(data),
                     "byte-exact" if data == original else "DIFFERS"))
        return 0 if data == original else 1

    if opts.diff:
        return savediff.diff(opts.path, opts.diff, opts.game, phases=opts.phases)
    if opts.set:
        try:
            data, report = savefile.patch(opts.path, opts.game, opts.set)
        except ValueError as exc:
            print("save: %s" % exc, file=sys.stderr)
            return 1
        for line in report:
            print("  " + line)
        if opts.out:
            with _wopen(opts.out, opts) as fh:
                fh.write(data)
            print("written: %s" % opts.out)
            return 0
        if opts.inplace:
            with open(opts.path, "wb") as fh:
                fh.write(data)
            print("written in place: %s" % opts.path)
            return 0
        print("dry run: pass --out FILE (or --inplace) to write", file=sys.stderr)
        return 1
    try:
        info = savefile.inspect(opts.path, opts.game)
    except ValueError as exc:
        print("save: %s" % exc, file=sys.stderr)
        return 1
    if opts.json:
        print(json.dumps(info, indent=2))
        return 0
    print("%s: %d bytes, level %s, save name %r"
          % (info["file"], info["bytes"], info["level_name"], info["title"]))
    if "source_level" not in info:
        print("  source level not matched (try --game)", file=sys.stderr)
        return 1
    print("  source level: %s, state tail 0x%x (%d bytes)"
          % (info["source_level"], info["state_tail"][0], info["state_tail"][1]))
    d = info.get("doors")
    if d:
        states = {}
        for r in d["records"]:
            states[r["state"]] = states.get(r["state"], 0) + 1
        print("  doors: %d (states %s)" % (len(d["records"]), states))
        for r in d["records"]:
            if r["state"]:
                print("    door %2d ids=%s state=%+d"
                      % (r["index"], r["ids"], r["state"]))
    s = info.get("switches")
    if s:
        print("  switches: %d records" % len(s["records"]))
    p = info.get("player")
    if p:
        print("  player: armor %.2f/%.2f, shields %s"
              % (p["armor"], p["armor_max"],
                 " ".join("%s=%.2f" % (n, p["shields"][n])
                          for n in savefile.SHIELD_NAMES)))
        names = p.get("names") or []
        sel = p["weapon_slot"]
        print("    weapons: mask 0x%x, selected %s"
              % (p["weapon_mask"],
                 "%d (%s)" % (sel, names[sel]) if 0 <= sel < len(names) else sel))
        cap = p.get("ammo_max") or []
        for i in sorted(p["ammo"]):
            if p["ammo"][i]:
                top = cap[i] if i < len(cap) else None
                print("      ammo %-16s %d%s"
                      % (names[i] if i < len(names) else "slot %d" % i,
                         p["ammo"][i], " / %d" % top if top else ""))
    return 0



INVENTORY = {
    ".sbw": ("gm/sbw.py", "чтение + запись + рендер"),
    ".sbt": ("gm/sbt.py", "чтение + запись, VM, сцена"),
    ".sbc": ("gm/sbc.py", "чтение + запись"),
    ".sob": ("gm/sob.py", "чтение + OBJ + рендер"),
    ".soo": ("gm/soo.py", "чтение + запись (текст)"),
    ".sof": ("gm/sof.py", "тайлы 8 КБ, кадр 128x64 (research)"),
    ".fdx": ("gm/sof.py", "индекс фильма (текст)"),
    ".sbm": ("gm/sbm.py", "чтение + запись + PNG/BMP"),
    ".sou": ("gm/sou.py", "контейнер + SOU0; SOU1 (ADPCM) и SOU2 (формат 4) декодируются"),
    ".sog": ("gm/sog.py", "чтение + запись"),
    ".soa": ("gm/soa.py", "чтение + запись"),
    ".soe": ("gm/soe.py", "чтение + запись"),
    ".sml": ("gm/sml.py", "чтение + запись"),
    ".sme": ("gm/sml.py", "чтение + запись"),
    ".sbv": ("gm/sbv.py", "декодер + контейнер"),
    ".bmp": ("gm/bmp.py", "чтение"),
    ".tab": ("gm/look.py", "разобрано"),
    ".slu": ("gm/scene.py", "разобрано"),
    ".dat": ("gm/bigfile.py", "контейнер BIGFILE (см. ниже)"),
    ".idx": ("gm/bigfile.py", "индекс BIGFILE"),
    ".dxw": ("—", "не игровой (DXWnd)"),
    ".png": ("—", "не игровой"),
    ".jpg": ("—", "не игровой"),
    ".html": ("—", "не игровой"),
    ".css": ("—", "не игровой"),
    ".txt": ("—", "не игровой"),
    ".docx": ("—", "не игровой"),
    ".rar": ("—", "не игровой"),
    ".bat": ("—", "не игровой"),
    ".ico": ("—", "не игровой"),
    ".isu": ("—", "не игровой"),
    ".pin": ("gm/look.py", "NEONS.PIN — разобран"),
    ".skk": ("gm/small.py", "чтение + запись (текст)"),
    ".skb": ("gm/small.py", "чтение + запись (текст)"),
    ".sdb": ("—", "база патча (не игровой)"),
    ".hi": ("gm/small.py", "чтение + запись (рекорды)"),
    ".sni": ("gm/small.py", "чтение + запись (текст)"),
    ".key": ("gm/small.py", "чтение + запись (376 Б)"),
    ".sub": ("gm/small.py", "чтение + запись (текст)"),
    ".cfg": ("gm/config.py", "1.93: чтение + запись, поля размечены; CONTROLS — opaque"),
    ".exe": ("tools/re", "PE-карта, дизасм, xref"),
    ".dll": ("—", "бинарь/система"),
    ".pif": ("—", "не игровой"),
    ".ini": ("—", "не игровой"),
    ".log": ("—", "не игровой"),
    ".tmp": ("—", "не игровой"),
    ".bak": ("—", "не игровой"),
    ".conf": ("—", "не игровой"),
    ".js": ("—", "не игровой"),
    ".wav": ("—", "не игровой"),
}



def cmd_manifest(opts) -> int:
    """SHA-256 manifest of the installation (names + hashes only, no data)."""
    import hashlib
    root = opts.path or opts.game
    if not os.path.isdir(root):
        print("manifest: %s is not a directory" % root, file=sys.stderr)
        return 1
    game_only = not opts.all
    rows = []
    total = 0
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            ext = os.path.splitext(name)[1].lower()
            module, status = INVENTORY.get(ext, ("—", "не разобран"))
            if game_only and status.startswith("не игровой"):
                continue
            try:
                size = os.path.getsize(path)
                h = hashlib.sha256()
                with open(path, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
            except OSError as exc:
                print("  FAIL %s: %s" % (path, exc), file=sys.stderr)
                continue
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if game_only and rel.split("/")[0].upper() == "SAVEGAME":
                continue          # user saves, not shipped assets
            rows.append((rel, h.hexdigest(), size))
            total += size
    rows.sort()
    digest = hashlib.sha256()
    for rel, h, size in rows:
        digest.update(("%s  %s\n" % (h, rel)).encode("utf-8"))
    lines = ["%s  %s" % (h, rel) for rel, h, _size in rows]
    if opts.out:
        os.makedirs(os.path.dirname(os.path.abspath(opts.out)), exist_ok=True)
        with open(opts.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
    if opts.json:
        print(json.dumps({"root": root, "files": len(rows), "bytes": total,
                          "digest": digest.hexdigest(),
                          "entries": [{"path": r, "sha256": h, "bytes": s}
                                      for r, h, s in rows]}, indent=2))
    else:
        print("%s: %d files, %.1f MiB, manifest digest %s"
              % (root, len(rows), total / 1048576, digest.hexdigest()))
        if opts.out:
            print("  written: %s" % opts.out)
    return 0


def cmd_inventory(opts) -> int:
    """Map every file in the installation: extension, count, bytes, module."""
    root = opts.path or opts.game
    if not os.path.isdir(root):
        print("inventory: %s is not a directory" % root, file=sys.stderr)
        return 1
    stats: dict[str, list] = {}
    total_files = total_bytes = 0
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            ext = os.path.splitext(name)[1].lower()
            rec = stats.setdefault(ext, [0, 0])
            rec[0] += 1
            rec[1] += size
            total_files += 1
            total_bytes += size
    rows = []
    for ext, (count, size) in stats.items():
        module, status = INVENTORY.get(ext, ("—", "не разобран"))
        rows.append({"ext": ext or "(без расширения)", "files": count,
                     "bytes": size, "module": module, "status": status})
    rows.sort(key=lambda r: -r["files"])
    if opts.json:
        pairs = []
        from gm import bigfile
        for base, _dirs, files in os.walk(root):
            for name in sorted(files):
                if not name.lower().endswith(".idx"):
                    continue
                path = os.path.join(base, name)
                try:
                    bf = bigfile.BigFile(path)
                except Exception:
                    continue
                pairs.append({"idx": os.path.relpath(path, root),
                              "entries": len(bf.entries),
                              "bytes": bf.dat_size})
        print(json.dumps({"root": root, "files": total_files,
                          "bytes": total_bytes, "rows": rows,
                          "containers": pairs}, indent=2, ensure_ascii=False))
        return 0
    print("%s: %d files, %.1f MiB" % (root, total_files, total_bytes / 1048576))
    print("  %-14s %7s %12s  %-14s %s"
          % ("ext", "files", "MiB", "module", "status"))
    for r in rows:
        print("  %-14s %7d %12.1f  %-14s %s"
              % (r["ext"], r["files"], r["bytes"] / 1048576,
                 r["module"], r["status"]))
    unknown = [r for r in rows if r["status"] == "не разобран"]
    print("  unknown extensions: %d (%d files)"
          % (len(unknown), sum(r["files"] for r in unknown)))
    # BIGFILE pairs: the payloads live in .DAT, so they are not in the walk
    from gm import bigfile
    pairs = []
    for base, _dirs, files in os.walk(root):
        for name in sorted(files):
            if not name.lower().endswith(".idx"):
                continue
            path = os.path.join(base, name)
            try:
                bf = bigfile.BigFile(path)
            except Exception:
                continue
            pairs.append((os.path.relpath(path, root), len(bf.entries),
                          bf.dat_size))
    if pairs:
        print("  BIGFILE containers (payloads counted separately):")
        for rel, count, size in pairs:
            print("    %-24s %6d entries %10.1f MiB"
                  % (rel, count, size / 1048576))
    return 0


def cmd_sbt(opts) -> int:
    ok = bad = 0
    for p in _sbt_iter(opts.paths):
        try:
            s = sbt.parse_file(p)
        except ValueError as exc:
            bad += 1
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            continue
        ok += 1
        print(f"{os.path.basename(p):14s} {os.path.getsize(p):8d} bytes, "
              f"header={s.header_size}, {len(s.records)} records")
        if opts.verbose:
            for r in s.records:
                print(f"    {r.offset:#08x} {r.name:20s} {r.size:7d}  {_sbt_brief(r.payload)}")
        if opts.rebuild:
            os.makedirs(opts.rebuild, exist_ok=True)
            out = os.path.join(opts.rebuild, os.path.basename(p))
            with _wopen(out, opts) as f:
                f.write(s.to_bytes())
        if opts.resolve:
            sbw_path = os.path.splitext(p)[0] + ".sbw"
            if not os.path.exists(sbw_path):
                print(f"  resolve: no {os.path.basename(sbw_path)} next to the .sbt, skipped")
            else:
                summary, problems = _sbt_resolve(s, sbw_path)
                for name, (total, good) in summary.items():
                    flag = "ok" if good == total else f"{total - good} BAD"
                    print(f"  resolve: {name:14s} {good}/{total} in range ({flag})")
                for line in problems[:50]:
                    print(f"  resolve:   {line}")
                if len(problems) > 50:
                    print(f"  resolve:   ... {len(problems) - 50} more")
                if problems:
                    bad += 1
        if opts.json:
            os.makedirs(opts.json, exist_ok=True)
            stem = os.path.splitext(os.path.basename(p))[0]
            with open(os.path.join(opts.json, stem + ".json"), "w") as f:
                json.dump(_sbt_json(s), f, indent=1)
        if opts.script:
            for r in s.records_of(11):
                code = r.payload.code
                insns, end = sbtvm.decode(code)
                status = "ok" if end == len(code) else f"END {end}/{len(code)}"
                stem = os.path.splitext(os.path.basename(p))[0]
                text = sbtvm.render(code)
                if opts.script_out:
                    os.makedirs(opts.script_out, exist_ok=True)
                    with open(os.path.join(opts.script_out, stem + ".script.txt"), "w") as f:
                        f.write(text + "\n")
                else:
                    head = text.splitlines()
                    shown = head[:opts.script_lines]
                    for line in shown:
                        print("   " + line)
                    if len(head) > len(shown):
                        print(f"   ... {len(head) - len(shown)} more lines "
                              f"(use --script-out DIR)")
                print(f"  script: {len(code)} bytes, {len(insns)} top-level ops, {status}"
                      + (f" -> {opts.script_out}" if opts.script_out else ""))
    print(f"sbt: {ok} parsed, {bad} failed"
          + (f" -> {opts.json}" if opts.json else ""))
    return 0 if bad == 0 else 1


def _sbt_brief(payload) -> str:
    if isinstance(payload, (sbt.Sprites, sbt.SoundSources)):
        return f"count={payload.count}"
    if isinstance(payload, sbt.SectorStuff):
        return f"{len(payload.raw)} bytes"
    if isinstance(payload, sbt.Item):
        return f"{len(payload.raw)} bytes"
    if isinstance(payload, (sbt.Thing, sbt.PlayerStatus)):
        return f"exists={payload.exists}" + (f" +{len(payload.raw)}b" if payload.raw else "")
    if isinstance(payload, sbt.Script):
        return f"things={payload.count} handles={payload.n} code={payload.m} flag={payload.flag}"
    if isinstance(payload, sbt.SampleTable):
        return f"nonzero={sum(1 for x in payload.names if x)}/300"
    if isinstance(payload, sbt.HandleTable):
        return f"n={len(payload)}"
    if isinstance(payload, sbt.Lift):
        return f"lifts={payload.lifts} stops={payload.stops}"
    if isinstance(payload, sbt.ThingTypes):
        return f"n={len(payload.entries)} first={payload.entries[0].name if payload.entries else '-'}"
    if isinstance(payload, sbt.NamedItems):
        return f"n={len(payload.entries)} first={payload.entries[0].name if payload.entries else '-'}"
    if isinstance(payload, sbt.SobTable):
        return f"n={len(payload.entries)}"
    if isinstance(payload, (sbt.Switch, sbt.Door)):
        return f"count={payload.count}"
    return ""


def _vec3_opt(text, name):
    """Parse a ``x,y,z`` command line value (None stays None)."""
    if not text:
        return None
    parts = text.replace(";", ",").split(",")
    if len(parts) != 3:
        print(f"{name} wants three comma-separated numbers", file=sys.stderr)
        raise SystemExit(2)
    return tuple(float(p) for p in parts)


def cmd_view(opts) -> int:
    import math

    from gm import render, sbw, scene as scenemod

    if opts.size < 16:
        print("size must be >= 16", file=sys.stderr)
        return 2
    az, el = opts.az, opts.el
    presets = {"front": (0.0, 0.0), "back": (180.0, 0.0), "left": (90.0, 0.0),
               "right": (-90.0, 0.0), "top": (0.0, 89.0), "iso": (35.0, 22.0)}
    if opts.angle:
        if opts.angle not in presets:
            print(f"unknown angle {opts.angle!r} (use {', '.join(presets)})",
                  file=sys.stderr)
            return 2
        az, el = presets[opts.angle]

    pool = None if opts.no_textures else render.TexturePool(opts.game, _pal(opts))
    kind_opts = dict(zoom=opts.zoom, uv=opts.uv, wire=opts.wire, cull=opts.cull,
                     lit=not opts.flat, materials=not opts.no_materials,
                     brightness=not opts.no_brightness, clip=opts.clip,
                     lightmap=not opts.no_lightmap, ambient=opts.ambient,
                     sectors=not opts.no_sectors, only_sector=opts.sector,
                     mip=opts.mip, filter=opts.filter,
                     alpha_test=opts.alpha_test,
                     transparent_index=opts.transparent_index)
    pos = _vec3_opt(opts.pos, "--pos")
    look = _vec3_opt(opts.look, "--look")
    if pos is None and look is not None:
        print("--look needs --pos", file=sys.stderr)
        return 2
    if opts.spawn and pos is not None:
        print("--spawn and --pos are mutually exclusive", file=sys.stderr)
        return 2
    paths: list[str] = []
    for p in opts.paths:
        if os.path.isdir(p):
            paths += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.lower().endswith((".sob", ".sbw"))]
        else:
            paths.append(p)
    if not paths:
        print("nothing to render", file=sys.stderr)
        return 2

    out_is_dir = (opts.out is None or len(paths) > 1
                  or opts.out.endswith(("/", os.sep))
                  or (not opts.out.lower().endswith(".png")
                      and not os.path.isfile(opts.out)))
    if out_is_dir:
        os.makedirs(opts.out or ".", exist_ok=True)

    rc = 0
    for p in paths:
        ext = os.path.splitext(p)[1].lower()
        markers = lights = None
        try:
            if ext == ".sbw":
                w = sbw.parse_file(p)
                lms = None
                sbc_path = os.path.splitext(p)[0] + ".sbc"
                if not opts.no_lightmap and os.path.isfile(sbc_path):
                    lms = render.import_lightmaps(sbc_path)
                if opts.spawn:
                    sbt_path = os.path.splitext(p)[0] + ".sbt"
                    sc0 = scenemod.load(sbt_path)
                    starts = [s for s in sc0.spawns
                              if s.kind == "campaign" and s.index == opts.spawn_slot]
                    if not starts:
                        raise ValueError("no campaign spawn for slot %d"
                                         % opts.spawn_slot)
                    eye0, look0, _basis = scenemod.first_person(
                        starts[0], opts.spawn_eye)
                    pos, look = eye0, look0
                meshes = render.level_meshes(w, lms,
                                             transform=opts.transform or opts.spawn)
                kind = f"level {len(w.sectors)}s/{w.n_objects}o"
                if lms:
                    kind += f" +{len(lms.records)} lightmaps"
                if opts.scene or opts.scene_lights:
                    scale = scenemod.SCALE if opts.scene_scale is None else opts.scene_scale
                    sc = scenemod.load(os.path.splitext(p)[0] + ".sbt")
                    cnt = sc.counts()
                    kind += (f" +scene {cnt['lights']}L/{cnt['locations']}T/"
                             f"{cnt['items']}I/{cnt['sounds']}S/{cnt['sprites']}P")
                    if opts.scene:
                        markers = sc.markers(scale)
                    if opts.scene_lights:
                        lights = sc.render_lights(scale)
                if opts.things:
                    scale = scenemod.SCALE if opts.scene_scale is None else opts.scene_scale
                    sc = scenemod.load(os.path.splitext(p)[0] + ".sbt")
                    placed = _place_things(sc, opts.game, scale, opts.thing_scale,
                                           meshes, render)
                    kind += f" +{placed}/{len(sc.things)} things"
                if opts.no_background:
                    keep = [m for m in meshes if not _is_background(m, 4000.0)]
                    dropped = len(meshes) - len(keep)
                    meshes = keep
                    kind += f" -{dropped} bg"
            elif ext == ".soo":
                from gm import soo as soomod
                anim = soomod.parse(open(p, "rb").read())
                pose = anim.frame_at(opts.at)
                if opts.at < 0:
                    pose = anim.frame_at(0)
                if opts.game and getattr(cmd_view, "_model_index", None) is None:
                    root = os.path.join(opts.game, "SOBS")
                    idx = {}
                    if os.path.isdir(root):
                        for n in os.listdir(root):
                            if n.lower().endswith(".sob"):
                                idx[os.path.splitext(n)[0].lower()] = os.path.join(root, n)
                    cmd_view._model_index = idx
                index = getattr(cmd_view, "_model_index", {}) or {}

                def _pose(name):
                    path = index.get(name.lower(), os.path.join(opts.game or "", "SOBS",
                                                                name + ".sob"))
                    return sob.parse(path)

                a = _pose(pose.name)
                b = _pose(pose.next_name)
                m = render.morph_models(a, b, pose.alpha)
                meshes = render.model_meshes(m)
                kind = (f"anim mode={anim.mode} frame {pose.index}/{pose.next_index} "
                        f"alpha={pose.alpha:.2f} at {opts.at} ms")
            else:
                m = sob.parse(p)
                meshes = render.model_meshes(m)
                kind = f"model {len(m.vertices)}v/{len(m.faces)}f"
        except Exception as exc:  # noqa: BLE001 - keep going over a batch
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            rc = 1
            continue
        n_faces = sum(len(m.faces) for m in meshes)
        t0 = time.time()
        if opts.sheet:
            data = render.contact_sheet(meshes, opts.size, pool=pool, **kind_opts)
        else:
            data = render.render(meshes, opts.size, pool=pool, az=az, el=el,
                                 pos=pos, look=look, markers=markers, lights=lights,
                                 marker_depth=opts.scene_markers_depth, **kind_opts)
        dt = time.time() - t0
        name = os.path.splitext(os.path.basename(p))[0]
        out = os.path.join(opts.out, name + ".png") if out_is_dir else opts.out
        with _wopen(out, opts) as f:
            f.write(data)
        tex_used = len({fc.texture for m in meshes for fc in m.faces})
        print(f"{os.path.basename(p)}: {kind}, {n_faces} faces, "
              f"{tex_used} textures, az={az:g} el={el:g}, "
              f"coverage {render.coverage(data):.1%}, {dt:.2f}s -> {out}")
        if opts.ascii:
            print(render.ascii_preview(data, opts.ascii))
    return rc


def _is_background(mesh, limit: float) -> bool:
    """True when a mesh spans more than *limit* along any axis (sky/backdrop)."""
    vs = getattr(mesh, "vertices", ())
    if not vs:
        return False
    for k in range(3):
        lo = min(v[k] for v in vs)
        hi = max(v[k] for v in vs)
        if hi - lo > limit:
            return True
    return False


def _place_things(sc, game, scale, thing_scale, meshes, render) -> int:
    """Append the models of the scene's things to *meshes*; return how many.

    Only things with an absolute position and a resolved ``SOB`` are placed.
    ``SOBS/<model>.sob`` is tried first, then ``Sobanim/<model>.soo``; the
    models are cached by name.  Orientation doubles are *not* applied (their
    meaning is unproven), so the models are only translated -- ``--thing-scale``
    multiplies the model size if the shipped assets need it.
    """
    if not game:
        return 0
    from gm import sob as sobmod

    cache: dict = {}
    placed = 0
    for t in sc.things:
        if not t.absolute or not t.model:
            continue
        if t.model not in cache:
            m = None
            for sub in ("SOBS", "Sobanim"):
                for ext in (".sob", ".soo"):
                    path = os.path.join(game, sub, t.model + ext)
                    if os.path.isfile(path):
                        try:
                            m = sobmod.parse(path)
                        except Exception:       # noqa: BLE001 - skip bad model
                            m = None
                        break
                if m is not None:
                    break
            cache[t.model] = m
        m = cache[t.model]
        if m is None or not m.vertices:
            continue
        pos = tuple(v * scale for v in t.pos)
        meshes.append(render.place_model(m, pos, thing_scale))
        placed += 1
    return placed


def cmd_scene(opts) -> int:
    """Dump the scripted scene (lights / locations / items / sounds) of .sbt files."""
    import json

    from gm import scene as scenemod

    paths: list[str] = []
    for p in opts.paths:
        if os.path.isdir(p):
            paths += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.lower().endswith(".sbt")]
        else:
            paths.append(p)
    if not paths:
        print("nothing to read", file=sys.stderr)
        return 2
    out = {}
    rc = 0
    for p in paths:
        name = os.path.splitext(os.path.basename(p))[0]
        try:
            sc = scenemod.load(p)
        except Exception as exc:  # noqa: BLE001 - keep going over a batch
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            rc = 1
            continue
        cnt = sc.counts()
        if not opts.json:
            print(f"{os.path.basename(p)}: {cnt['lights']} lights, "
                  f"{cnt['locations']} locations/things, {cnt['items']} items, "
                  f"{cnt['sounds']} sound sources, {cnt['sprites']} sprites, "
                  f"{cnt['things']} things "
                  f"({sum(1 for t in sc.things if t.absolute)} placed, "
                  f"{len({t.model for t in sc.things if t.model})} models), "
                  f"{cnt['doors']} doors, {cnt['switches']} switches, "
                  f"{cnt['lifts']} lifts, {cnt['spawns']} spawns, "
                  f"{cnt['other']} other placement commands, "
                  f"{cnt['entities']} entities")
            for l in sc.lights[:opts.show]:
                print(f"  light {l.kind:5s} pos=({l.pos[0]},{l.pos[1]},{l.pos[2]}) "
                      f"rgb={l.rgb} param={l.param}")
            for x in sc.locations[:opts.show]:
                print(f"  location id={x.thing_id} pos={x.pos} yaw={x.yaw:.3f}")
            for x in sc.items[:opts.show]:
                print(f"  item id={x.item_id} pos={x.pos} fixed={x.fixed}")
            for x in sc.sounds[:opts.show]:
                print(f"  sound pos={x.pos} sample={x.sample} volume={x.volume}")
            for x in sc.sprites[:opts.show]:
                print(f"  sprite id={x.id} pos=({x.pos[0]:g},{x.pos[1]:g},{x.pos[2]:g}) "
                      f"jitter={x.jitter}")
            for t in sc.things[:opts.show]:
                pos = "(%d,%d,%d)" % t.pos if t.absolute else f"@{t.kind}"
                print(f"  thing {t.kind} type={t.type_index:3d} {t.type_name:12s} "
                      f"model={t.model or '-':10s} pos={pos} doubles="
                      f"({','.join('%g' % a for a in t.angles)})")
            for x in sc.doors[:opts.show]:
                print(f"  door flags={x.flags} ids={x.ids} state={x.state} "
                      f"pos=({','.join('%g' % v for v in x.pos)}) "
                      f"{'sliding ' if x.moving else ''}"
                      f"angles=({','.join('%g' % a for a in x.angles)})")
            for x in sc.switches[:opts.show]:
                print(f"  switch id={x.id} kind={x.kind} pos={x.pos} "
                      f"radius={x.radius}")
            for x in sc.lifts[:opts.show]:
                print(f"  lift stops={len(x.stops)} head={len(x.head)}B")
            for s in sc.spawns[:opts.show]:
                print(f"  spawn {s.kind:10s} slot={s.index} pos={s.pos} "
                      f"yaw={s.angles[2]:.3f}")
            if sc.other:
                top = sorted(sc.other.items(), key=lambda kv: -kv[1])[:4]
                print(f"  other: {', '.join(f'{k}x{v}' for k, v in top)}")
        out[name] = {
            "lights": [{"kind": l.kind, "pos": list(l.pos), "rgb": list(l.rgb),
                        "param": l.param, "extra": list(l.extra)} for l in sc.lights],
            "locations": [{"id": x.thing_id, "pos": list(x.pos),
                           "orientation": list(x.orientation)} for x in sc.locations],
            "items": [{"id": x.item_id, "pos": list(x.pos), "fixed": x.fixed,
                       "extra": list(x.extra)} for x in sc.items],
            "sounds": [{"pos": list(x.pos), "sample": x.sample, "volume": x.volume,
                        "pitch": x.pitch} for x in sc.sounds],
            "sprites": [{"id": x.id, "pos": list(x.pos), "jitter": x.jitter,
                         "extra": list(x.extra)} for x in sc.sprites],
            "things": [{"entity": t.entity, "kind": t.kind,
                        "type": t.type_index, "type_name": t.type_name,
                        "model": t.model, "pos": list(t.pos) if t.absolute else None,
                        "angles": list(t.angles), "owner": t.owner}
                       for t in sc.things],
            "doors": [{"flags": d.flags, "ids": list(d.ids),
                       "angles": list(d.angles), "params": list(d.params),
                       "sector": d.sector, "object": d.object,
                       "pos": list(d.pos), "open_pos": list(d.open_pos),
                       "size": list(d.size), "moving": d.moving,
                       "values": list(d.values), "tail": list(d.tail)}
                      for d in sc.doors],
            "switches": [{"kind": s.kind, "id": s.id, "radius": s.radius,
                          "pos": list(s.pos), "extra": list(s.extra)}
                         for s in sc.switches],
            "lifts": [{"stops": [list(x) for x in l.stops],
                       "head": len(l.head)} for l in sc.lifts],
            "spawns": [{"slot": s.index, "pos": list(s.pos),
                        "angles": list(s.angles), "kind": s.kind}
                       for s in sc.spawns],
            "entities": [{"id": i, "flags": f, "offset": o}
                         for i, f, o in sc.entities],
            "other": sc.other,
        }
    if opts.json:
        json.dump(out, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    elif opts.out:
        with open(opts.out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1, sort_keys=True)
        print(f"scene: {len(out)} levels -> {opts.out}")
    return rc


EYE_HEIGHTS = {"LIGHT": 10.0, "MEDIUM": 11.0, "STEALTH": 11.0,
               "HEAVY": 12.0, "57 CHEVY": 14.0}


def cmd_spawn(opts) -> int:
    """Player starts (SET_PLAYER_START) and DM respawn points of .sbt levels."""
    import json

    from gm import scene as scenemod

    paths: list[str] = []
    for p in opts.paths:
        if os.path.isdir(p):
            paths += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.lower().endswith(".sbt")]
        else:
            paths.append(p)
    if not paths:
        print("nothing to read", file=sys.stderr)
        return 2
    eye = opts.eye
    if eye is None:
        eye = EYE_HEIGHTS.get((opts.chassis or "LIGHT").upper(), 16.0)
    out = {}
    rc = 0
    for p in paths:
        name = os.path.splitext(os.path.basename(p))[0]
        try:
            sc = scenemod.load(p)
        except Exception as exc:  # noqa: BLE001 - keep going over a batch
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            rc = 1
            continue
        campaign = [s for s in sc.spawns
                    if s.kind == "campaign" and s.index == opts.slot]
        deathmatch = [s for s in sc.spawns if s.kind == "deathmatch"]
        out[name] = {
            "eye_height": eye,
            "campaign": [dict(
                {"slot": s.index, "pos": list(s.pos), "angles": list(s.angles)},
                **dict(zip(("eye", "look", "basis"),
                           scenemod.first_person(s, eye))))
                for s in campaign],
            "deathmatch": [{"slot": s.index, "pos": list(s.pos),
                            "angles": list(s.angles)} for s in deathmatch],
        }
        if not opts.json:
            print(f"{os.path.basename(p)}: {len(campaign)} campaign start(s), "
                  f"{len(deathmatch)} DM start(s), eye +{eye:g}")
            for s in campaign[:opts.show]:
                eye_pos, _look, basis = scenemod.first_person(s, eye)
                print(f"  campaign slot={s.index} pos={s.pos} "
                      f"yaw={s.angles[2]:.3f} camera={eye_pos} "
                      f"forward=({basis['forward'][0]:+.3f},"
                      f"{basis['forward'][1]:+.3f},{basis['forward'][2]:+.3f})")
            for s in deathmatch[:opts.show]:
                print(f"  deathmatch slot={s.index} pos={s.pos} "
                      f"yaw={s.angles[2]:.3f}")
    if opts.json:
        json.dump(out, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    return rc


def cmd_sob(opts) -> int:
    if not opts.check and not opts.out and not opts.rebuild:
        print("sob: OUT is required unless --check or --rebuild", file=sys.stderr)
        return 2
    files = sorted(
        os.path.join(opts.src, f) for f in os.listdir(opts.src) if f.lower().endswith(".sob")
    )
    if opts.rebuild:
        os.makedirs(opts.rebuild, exist_ok=True)
    ok = 0
    errs = 0
    for p in files:
        try:
            if opts.rebuild:
                dst = os.path.join(opts.rebuild, os.path.basename(p))
                sob.parse(p).write(dst, force=opts.force, exact=opts.exact)
            elif opts.check:
                sob.parse(p)
            else:
                sob.to_obj_file(p, opts.out)
            ok += 1
        except Exception as exc:  # noqa: BLE001 - report and continue
            errs += 1
            print(f"  FAIL {os.path.basename(p)}: {exc}", file=sys.stderr)
    if opts.rebuild:
        print(f"sob: {ok} rebuilt ({'byte-exact' if opts.exact else 'canonical'}), "
              f"{errs} failed -> {opts.rebuild}")
    elif opts.check:
        print(f"sob: {ok} parsed, {errs} failed")
    else:
        print(f"sob: {ok} converted, {errs} failed -> {opts.out}")
    return 0 if errs == 0 else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gm.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list entries of a BIGFILE archive")
    p.add_argument("idx")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("extract", help="dump raw blobs of a BIGFILE archive")
    p.add_argument("idx")
    p.add_argument("out")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("pack", help="build a BIGFILE .idx/.dat pair from a directory of files")
    p.add_argument("out", help="output .idx (the .DAT is written next to it)")
    p.add_argument("src", help="directory whose files become entries (name = file stem)")
    p.add_argument("--ext", help="only include files with this extension (e.g. .sbm)")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("set", help="replace/add one entry in a BIGFILE archive")
    p.add_argument("idx", help="source .idx to copy")
    p.add_argument("name", help="virtual entry name (1..8 bytes)")
    p.add_argument("file", help="payload file")
    p.add_argument("out", help="output .idx (the .DAT is written next to it)")
    p.set_defaults(func=cmd_set)

    p = sub.add_parser("sbm", help="encode an image (PNG/BMP) into an .sbm texture")
    p.add_argument("image")
    p.add_argument("out")
    p.add_argument("--palette", help="BMP providing the palette (overrides --game)")
    p.add_argument("--game", help="installation dir (for the shared palette)")
    p.add_argument("--mips", type=int, default=1, help="number of mip levels to store (default 1)")
    p.add_argument("--flags", type=int, default=0,
                   help="header flags (PICS 0/1/2/16; sprites 256/257; default 0)")
    p.add_argument("--filter", choices=["box", "nearest"], default="box",
                   help="mip downsampling filter (default box)")
    p.set_defaults(func=cmd_sbm)

    p = sub.add_parser("textures", help="PICS/SBM.DAT -> PNG")
    p.add_argument("game")
    p.add_argument("out")
    p.add_argument("--palette", help="BMP file to take the palette from")
    p.add_argument("--transparent", type=int, default=None, help="palette index to make transparent")
    p.set_defaults(func=cmd_textures)

    p = sub.add_parser("sprites", help="sprites/SBM.DAT -> PNG")
    p.add_argument("game")
    p.add_argument("out")
    p.add_argument("--palette", help="BMP file to take the palette from")
    p.add_argument("--transparent", type=int, default=0, help="palette index to make transparent (default 0)")
    p.set_defaults(func=cmd_sprites)

    p = sub.add_parser("sounds", help="sounds/SOU.DAT -> WAV (SOU0/SOU1/SOU2)")
    p.add_argument("game")
    p.add_argument("out")
    p.set_defaults(func=cmd_sounds)

    p = sub.add_parser("sou", help="SOU container stats / byte-exact rebuild")
    p.add_argument("idx", help="sounds/SOU.IDX or MUSIC/SOU.IDX")
    p.add_argument("--stats", action="store_true", help="print stats only")
    p.add_argument("--json", action="store_true")
    p.add_argument("--rebuild", metavar="DIR", help="write every blob back (.sou)")
    p.set_defaults(func=cmd_sou)

    p = sub.add_parser("music", help="MUSIC/SOU.IDX -> one WAV per SOU2 entry")
    p.add_argument("idx", help="music .IDX (the .DAT is its sibling)")
    p.add_argument("out")
    p.add_argument("--limit", type=int, help="decode only the first N entries")
    p.add_argument("--volume", type=int, default=sou2.MAX_VOLUME,
                   help=f"mixer volume 0..32 (default {sou2.MAX_VOLUME})")
    p.set_defaults(func=cmd_music)

    p = sub.add_parser("sbc", help=".sbc lightmap blobs -> per-record grayscale PNG + contact sheet")
    p.add_argument("paths", nargs="+", help="one or more .sbc files, or directories to scan")
    p.add_argument("out")
    p.add_argument("--rebuild", help="write each .sbc back (re-serialised) into this directory")
    p.add_argument("--check", action="store_true", help="parse only, write nothing")
    p.set_defaults(func=cmd_sbc)

    p = sub.add_parser("frontend", help="FRONTEND/BMP.DAT -> BMP")
    p.add_argument("game")
    p.add_argument("out")
    p.set_defaults(func=cmd_frontend)

    p = sub.add_parser("sob", help="convert .sob models to OBJ/MTL")
    p.add_argument("src")
    p.add_argument("out", nargs="?")
    p.add_argument("--check", action="store_true", help="parse only, write nothing")
    p.add_argument("--rebuild", metavar="DIR",
                   help="write every .sob back (lossless raw unless --canonical)")
    p.add_argument("--exact", action="store_true", default=True,
                   help="with --rebuild: unchanged models keep their raw bytes "
                        "(default)")
    p.add_argument("--canonical", dest="exact", action="store_false",
                   help="with --rebuild: always use the canonical token layout")
    p.set_defaults(func=cmd_sob)

    p = sub.add_parser("scene", help="dump the scripted scene of .sbt levels")
    p.add_argument("paths", nargs="+", help=".sbt files or directories")
    p.add_argument("--json", action="store_true", help="print JSON to stdout")
    p.add_argument("--out", help="write JSON to this file")
    p.add_argument("--show", type=int, default=3,
                   help="how many entries of each kind to print (default 3)")
    p.set_defaults(func=cmd_scene)

    p = sub.add_parser("spawn", help="player starts (SET_PLAYER_START) and DM starts")
    p.add_argument("paths", nargs="+", help=".sbt files or directories")
    p.add_argument("--json", action="store_true", help="print JSON to stdout")
    p.add_argument("--slot", type=int, default=0, help="player slot (default 0)")
    p.add_argument("--eye", type=float, default=None,
                   help="camera height above the spawn (overrides --chassis)")
    p.add_argument("--chassis", default="LIGHT",
                   help="LIGHT/MEDIUM/STEALTH/HEAVY/57 CHEVY (default LIGHT)")
    p.add_argument("--show", type=int, default=3,
                   help="how many starts to print (default 3)")
    p.set_defaults(func=cmd_spawn)

    p = sub.add_parser("view", help="render .sob models / .sbw levels to PNG")
    p.add_argument("paths", nargs="+", help=".sob/.sbw files or directories")
    p.add_argument("--out", help="output PNG file or directory (default: current dir)")
    p.add_argument("--size", type=int, default=480, help="image size in pixels (default 480)")
    p.add_argument("--angle", help="preset view: front/back/left/right/top/iso")
    p.add_argument("--az", type=float, default=35.0, help="azimuth in degrees (default 35)")
    p.add_argument("--el", type=float, default=22.0, help="elevation in degrees (default 22)")
    p.add_argument("--zoom", type=float, default=1.0, help="zoom factor (default 1)")
    p.add_argument("--uv", choices=("fit", "tile"), default="fit",
                   help="map the texture onto each face (default) or repeat per unit")
    p.add_argument("--no-textures", action="store_true", help="flat colours only")
    p.add_argument("--flat", action="store_true", help="no lighting (full brightness)")
    p.add_argument("--wire", action="store_true", help="overlay polygon edges")
    p.add_argument("--cull", action="store_true", help="backface culling")
    p.add_argument("--sheet", action="store_true",
                   help="render four preset angles into one PNG (iso/front/left/top)")
    p.add_argument("--pos", help="camera position 'x,y,z' (level interiors)")
    p.add_argument("--look", help="camera target 'x,y,z' (default: model centre)")
    p.add_argument("--clip", type=float,
                   help="drop faces farther than this many units from the eye")
    p.add_argument("--no-materials", action="store_true",
                   help="disable the blended material pass (translucent/add/...)")
    p.add_argument("--no-brightness", action="store_true",
                   help="ignore the polygon brightness field (0x70)")
    p.add_argument("--no-lightmap", action="store_true",
                   help="do not load/sample the sibling <level>.sbc lightmaps")
    p.add_argument("--mip", type=lambda s: s if s == "auto" else int(s), default=0,
                   help="mip level (0=base), 'auto' for screen-space LOD")
    p.add_argument("--filter", choices=("nearest", "bilinear"), default="nearest",
                   help="texture filtering (default nearest)")
    p.add_argument("--alpha-test", action="store_true",
                   help="drop texels with the transparent palette index")
    p.add_argument("--transparent-index", type=int, default=0,
                   help="palette index treated as transparent (default 0)")
    p.add_argument("--ambient", type=float, default=0.0,
                   help="constant light level added to polygons (0..127)")
    p.add_argument("--sector", type=int,
                   help="render only this level sector (geometry group) by index")
    p.add_argument("--no-sectors", action="store_true",
                   help="disable per-object frustum (view pyramid) culling")
    p.add_argument("--scene", action="store_true",
                   help="overlay the scripted scene entities of <level>.sbt "
                        "(lights / locations / items / sounds) as markers")
    p.add_argument("--scene-lights", action="store_true",
                   help="shade the polygons that have no lightmap from the "
                        "scripted light sources (approximation)")
    p.add_argument("--scene-scale", type=float,
                   help="scale scripted scene coordinates into the mesh frame "
                        "(default 1/1.875, the .sbw object basis)")
    p.add_argument("--scene-markers-depth", action="store_true",
                   help="depth-test the scene markers (hide them behind geometry)")
    p.add_argument("--transform", action="store_true",
                   help="place the .sbw objects in the engine world frame "
                        "(origin + basis*local from the 96-byte object header; "
                        "the default keeps the raw file frame)")
    p.add_argument("--at", type=int, default=0, metavar="MS",
                   help="with a .soo path: animation time in milliseconds")
    p.add_argument("--spawn", action="store_true",
                   help="camera at SET_PLAYER_START (sibling .sbt; implies --transform)")
    p.add_argument("--spawn-slot", type=int, default=0,
                   help="player slot for --spawn (default 0)")
    p.add_argument("--spawn-eye", type=float, default=10.0,
                   help="camera height above the spawn (default 10 = LIGHT)")
    p.add_argument("--things", action="store_true",
                   help="place the models of the level's things (ADD_THING* in "
                        "the entity sub-scripts) alongside the geometry")
    p.add_argument("--thing-scale", type=float, default=1.0,
                   help="extra scale for the placed thing models (default 1)")
    p.add_argument("--no-background", action="store_true",
                   help="drop the level's sky/background objects (any axis "
                        "longer than 4000 file units), e.g. for overviews")
    p.add_argument("--ascii", type=int, nargs="?", const=78, default=0,
                   help="also print a coarse ASCII preview (optional width)")
    p.add_argument("--palette", help="BMP file to take the palette from")
    p.add_argument("--game", help="installation dir (for textures and the palette)")
    p.set_defaults(func=cmd_view)

    p = sub.add_parser("sml", help="decode .sme briefings / dump .sml markup + plain text")
    p.add_argument("paths", nargs="+", help="the SML/ directory or .sml/.sme files")
    p.add_argument("out", help="output directory (sml/ markup and txt/ plain text)")
    p.add_argument("--rebuild", help="write the re-encoded .sme form into this directory")
    p.add_argument("-v", "--verbose", action="store_true", help="per-file summary")
    p.set_defaults(func=cmd_sml)

    p = sub.add_parser("soa", help="list .soa animation descriptors")
    p.add_argument("paths", nargs="+", help="one or more .soa files or directories")
    p.add_argument("--rebuild", help="write each .soa back (re-serialised) into this directory")
    p.add_argument("--timeline", action="store_true", help="print frame intervals (91 Hz ticks)")
    p.add_argument("--at", type=int, metavar="MS", help="frame active at MS milliseconds")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_soa)

    p = sub.add_parser("soe", help="parse .soe thing-type definitions (text)")
    p.add_argument("paths", nargs="*", help="one or more .soe files or directories")
    p.add_argument("--fields", action="store_true",
                   help="print the THING_TYPES field map (offset/kind/status/xrefs)")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="dump every field of the token stream")
    p.add_argument("--rebuild", help="write each .soe back (re-serialised) into this directory")
    p.set_defaults(func=cmd_soe)

    p = sub.add_parser("misc", help="inspect .sub, .skk/.skb, .sni, .key, .hi")
    p.add_argument("paths", nargs="+", help="files or a directory")
    p.add_argument("--json", action="store_true")
    p.add_argument("--rebuild", metavar="DIR", help="write re-serialised copies")
    p.set_defaults(func=cmd_misc)

    p = sub.add_parser("film", help="inspect .sof HUD films (8 KB tiles) and "
                                     "their .fdx indexes")
    p.add_argument("paths", nargs="+", help=".sof files or the Film directory")
    p.add_argument("--json", action="store_true")
    p.add_argument("--frame", type=int, default=0, help="tile to export")
    p.add_argument("--png", metavar="OUT",
                   help="write the tile as PNG (a file, or a directory for many)")
    p.add_argument("--palette", help="BMP whose palette to apply to --png")
    p.add_argument("--apng", metavar="OUT",
                   help="write the film as animated PNG (needs the VGA palette)")
    p.add_argument("--fps", type=int, default=0,
                   help="APNG fps (default: sibling .fdx line or 24)")
    p.add_argument("--limit-frames", type=int,
                   help="APNG: write only the first N frames (testing)")
    p.add_argument("--rebuild", metavar="DIR",
                   help="write every .sof (and its .fdx) back, byte-exact")
    p.set_defaults(func=cmd_film)

    p = sub.add_parser("soo", help="parse .soo flipbook animations (text)")
    p.add_argument("paths", nargs="+", help=".soo files or the Sobanim directory")
    p.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx",
                   help="installation, to check frame models against SOBS/")
    p.add_argument("--json", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="list every frame")
    p.add_argument("--rebuild", metavar="DIR", help="write re-serialised copies")
    p.add_argument("--timeline", action="store_true",
                   help="print loop/one-shot mode and the engine frame intervals")
    p.add_argument("--at", type=int, metavar="MS",
                   help="print the interpolated pose at MS milliseconds")
    p.set_defaults(func=cmd_soo)

    p = sub.add_parser("look", help="analyse LOOK16.TAB / LOOK162.TAB lookup cubes")
    p.add_argument("file")
    p.add_argument("--png", help="write the cube as an 8x8 montage PNG")
    p.add_argument("--palette", help="BMP to colourise the PNG (default: grey ramp)")
    p.add_argument("--neons", help="NEONS.PIN to cross-check against the value set")
    p.add_argument("--scale", type=int, default=1, help="integer upscale for --png")
    p.set_defaults(func=cmd_look)

    p = sub.add_parser("save", help="inspect a SAVEGAME/*.sbt save (doors, "
                                    "switches, player, ammo) or diff two saves")
    p.add_argument("path", help="a SAVEGAME/saved<N>.sbt file")
    p.add_argument("--diff", metavar="OTHER",
                   help="compare with another save of the same level")
    p.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    p.add_argument("--json", action="store_true", help="dump the decoded state as JSON")
    p.add_argument("--phases", action="store_true",
                   help="with --diff: also list phase-only door changes")
    p.add_argument("--set", action="append", metavar="KEY=VALUE",
                   help="edit the state (armor, armor_max, shield_max, "
                        "shield=f,b,l,r, selected, mask, ammo=SLOT:VALUE, "
                        "door=INDEX:STATE); repeatable")
    p.add_argument("--rebuild", action="store_true",
                   help="serialise the parsed state; with --out write it, "
                        "otherwise verify byte-exactness")
    p.add_argument("--out", metavar="FILE", help="with --set/--rebuild: write a copy")
    p.add_argument("--inplace", action="store_true",
                   help="with --set: overwrite the save (explicit!)")
    p.set_defaults(func=cmd_save)

    p = sub.add_parser("manifest", help="SHA-256 manifest of the installation "
                                        "(game files by default)")
    p.add_argument("path", nargs="?", help="directory (default: --game)")
    p.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    p.add_argument("--all", action="store_true",
                   help="include non-game files (DXWnd exports, docs, images)")
    p.add_argument("--out", metavar="FILE", help="write sha256sum-compatible lines")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_manifest)

    p = sub.add_parser("inventory", help="map the installation: extension, "
                                         "count, size, module, status")
    p.add_argument("path", nargs="?", help="directory (default: --game)")
    p.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_inventory)

    p = sub.add_parser("sog", help="parse .sog songs (text playlists)")
    p.add_argument("path", help="a .sog file or the MUSIC directory")
    p.add_argument("--music", help="MUSIC/SOU.IDX to resolve sample names against")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--rebuild", help="write each .sog back (re-serialised) into this directory")
    p.set_defaults(func=cmd_sog)

    p = sub.add_parser("sbv", help="parse .sbv video (header + frame blocks)")
    p.add_argument("file", help="a .sbv clip, or a raw-frame directory written by OUT")
    p.add_argument("out", nargs="?", help="output dir for raw frame blocks "
                   "(or, with a directory as FILE, the target .sbv to encode)")
    p.add_argument("--list-only", action="store_true")
    p.add_argument("--png", help="decode frames and write PNGs into this directory")
    p.add_argument("--frames", default="0", help="frames to decode for --png: "
                   "'0', '1,5,9-12' or 'all' (default: 0)")
    p.add_argument("--scale", type=int, default=1, help="integer upscale for --png (default: 1)")
    p.add_argument("--no-raw", action="store_true", help="do not write raw frame blocks to OUT")
    p.add_argument("--verify", action="store_true",
                   help="encode/decode the --frames selection and compare planes")
    p.add_argument("--rebuild", help="write the clip back (re-serialised) into this directory")
    p.set_defaults(func=cmd_sbv)

    p = sub.add_parser("sbw", help="parse .sbw world tables + geometry")
    p.add_argument("files", nargs="+")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--obj", help="also export the level geometry as .obj into this directory")
    p.add_argument("--transform", action="store_true", help="apply each object's header to its vertices in the .obj")
    p.add_argument("--rebuild", help="write each .sbw back (re-serialised) into this directory")
    p.set_defaults(func=cmd_sbw)

    p = sub.add_parser("sbt", help="parse .sbt world/level record streams")
    p.add_argument("paths", nargs="+", help="one or more .sbt files or directories")
    p.add_argument("-v", "--verbose", action="store_true", help="list every record")
    p.add_argument("--resolve", action="store_true",
                   help="check the handle tables against the sibling .sbw geometry")
    p.add_argument("--json", help="write the parsed structure as JSON into this directory")
    p.add_argument("--rebuild", help="write each .sbt back (re-serialised) into this directory")
    p.add_argument("--script", action="store_true",
                   help="decompile the SCRIPT (type 11) bytecode to readable text")
    p.add_argument("--script-out", help="write the decompiled mission text into this directory")
    p.add_argument("--script-lines", type=int, default=40,
                   help="lines of the decompiled script to preview on stdout (default 40)")
    p.set_defaults(func=cmd_sbt)

    for _sp in sub.choices.values():
        _sp.add_argument("--force", action="store_true",
                         help="overwrite existing output files (default: refuse)")

    opts = ap.parse_args(argv)
    try:
        return opts.func(opts)
    except FileExistsError as exc:
        print(f"gm.py: {exc}", file=sys.stderr)
        return 1
    except (ValueError, OSError, struct.error) as exc:
        # Malformed input is a normal outcome for this toolkit; report it as a
        # one-line error instead of a traceback (and keep the exit code 1).
        print(f"gm.py: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
