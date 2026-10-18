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
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gm import (  # noqa: E402
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
    sob,
    soa,
    soe,
    sog,
    sou,
)


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
    n = 0
    for e in bf.entries:
        blob = e.data(open(bf.dat_path, "rb").read())
        name = e.name or f"unnamed_{e.offset:08x}"
        with open(os.path.join(opts.out, name + ".bin"), "wb") as f:
            f.write(blob)
        n += 1
    print(f"wrote {n} blobs to {opts.out}")
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
        n, size = bigfile.pack(blobs, opts.out)
    except ValueError as exc:
        print(f"pack: {exc}", file=sys.stderr)
        return 1
    print(f"pack: {n} entries, {size} bytes -> {opts.out}")
    return 0


def cmd_set(opts) -> int:
    blob = open(opts.file, "rb").read()
    try:
        dat = bigfile.replace_entry(opts.idx, opts.name, blob, opts.out)
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
    with open(opts.out, "wb") as f:
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
        with open(os.path.join(opts.out, e.name + ".png"), "wb") as f:
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
        with open(os.path.join(opts.out, e.name + ".png"), "wb") as f:
            f.write(img.to_png(pal, opts.transparent))
        ok += 1
    print(f"sprites: {ok} PNG written, {bad} failed -> {opts.out}")
    return 0 if bad == 0 else 1


def cmd_sounds(opts) -> int:
    bf = bigfile.BigFile(os.path.join(opts.game, "sounds", "SOU.IDX"))
    os.makedirs(opts.out, exist_ok=True)
    dat = open(bf.dat_path, "rb").read()
    codecs: dict[str, int] = {}
    pcm = coded = bad = 0
    for e in bf.entries:
        try:
            snd = sou.parse(e.data(dat))
        except ValueError:
            bad += 1
            continue
        codecs[snd.magic] = codecs.get(snd.magic, 0) + 1
        if snd.is_pcm:
            with open(os.path.join(opts.out, e.name + ".wav"), "wb") as f:
                f.write(snd.to_wav())
            pcm += 1
        else:
            # Honest raw dump: the payload is a driver-side codec, not PCM, so
            # no WAV is written (see gm/sou.py and docs/FORMATS.md).
            tag = "mono" if snd.channels == 1 else "stereo"
            name = f"{e.name}.{snd.magic.lower()}.{snd.sample_rate}.{tag}.bin"
            with open(os.path.join(opts.out, name), "wb") as f:
                f.write(snd.payload)
            coded += 1
    print(f"sounds: {pcm} PCM WAV, {coded} coded payloads dumped raw, {bad} failed")
    print(f"  containers: {codecs}")
    return 0 if bad == 0 else 1


def cmd_frontend(opts) -> int:
    bf = bigfile.BigFile(os.path.join(opts.game, "FRONTEND", "BMP.IDX"))
    os.makedirs(opts.out, exist_ok=True)
    n = 0
    dat = open(bf.dat_path, "rb").read()
    for e in bf.entries:
        blob = e.data(dat)
        ext = ".bmp" if blob[:2] == b"BM" else ".bin"
        with open(os.path.join(opts.out, e.name + ext), "wb") as f:
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
            with open(dst, "wb") as f:
                f.write(blob.to_bytes())
        out = os.path.join(opts.out, stem)
        os.makedirs(out, exist_ok=True)
        for i, rec in enumerate(blob.records):
            with open(os.path.join(out, f"{i:04d}_{rec.width}x{rec.height}.png"), "wb") as f:
                f.write(rec.to_png())
        with open(os.path.join(out, "_sheet.png"), "wb") as f:
            f.write(_sbc_sheet(blob))
        ok += 1
    print(f"sbc: {ok} files, {bad} failed -> {opts.out}")
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


def cmd_soa(opts) -> int:
    n = bad = 0
    for p, rel in _iter_with_rel(opts.paths, ".soa"):
        try:
            a = soa.parse(open(p, "rb").read())
        except ValueError as exc:
            bad += 1
            print(f"  FAIL {p}: {exc}", file=sys.stderr)
            continue
        extra = "" if len(a.names) == a.frame_count else f" (+{len(a.names) - a.frame_count} extra)"
        print(f"{os.path.basename(p):16s} mode={a.mode} period={a.period} "
              f"loop={a.loop} frames={a.frame_count}{extra}")
        if opts.rebuild:
            dst = os.path.join(opts.rebuild, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(a.to_bytes())
        n += 1
    print(f"soa: {n} parsed, {bad} failed")
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
            with open(dst, "wb") as f:
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
            with open(dst, "wb") as f:
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


def cmd_sbv(opts) -> int:
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

    if opts.png:
        frames = _parse_frame_spec(opts.frames, clip.frame_count)
        os.makedirs(opts.png, exist_ok=True)
        W, H = clip.width * opts.scale, clip.height * opts.scale
        for i in frames:
            rgb = sbv.decode_frame_rgb(clip.frame(blob, i).data, clip.width, clip.height)
            if opts.scale != 1:
                rgb = _scale_rgb(rgb, clip.width, clip.height, opts.scale)
            out = os.path.join(opts.png, f"{i:05d}.png")
            with open(out, "wb") as f:
                f.write(png.rgb(W, H, rgb))
        print(f"sbv: {len(frames)} frame(s) decoded -> {opts.png}")
        if sbv.FAMILY_BLOCKS:
            print(f"warning: {sbv.FAMILY_BLOCKS} block(s) used an approximated "
                  f"class >= 2 code path", file=sys.stderr)

    if opts.out and not opts.no_raw:
        os.makedirs(opts.out, exist_ok=True)
        with open(os.path.join(opts.out, "frames.txt"), "w") as f:
            f.write("# index timestamp_ms offset size\n")
            prev = 0
            for i, (ts, end) in enumerate(clip.table):
                f.write(f"{i} {ts} {clip.data_start + prev} {end - prev}\n")
                prev = end
        for fr in clip.frames(blob):
            with open(os.path.join(opts.out, f"{fr.index:05d}.sbvf"), "wb") as f:
                f.write(fr.data)
        print(f"sbv: {clip.frame_count} frames -> {opts.out} (raw blocks + frames.txt)")
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
            with open(out, "wb") as f:
                f.write(w.to_bytes())
    print(f"sbw: {ok} parsed, {bad} failed")
    return 0 if bad == 0 else 1


def _sbt_iter(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _d, files in os.walk(p):
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
            with open(out, "wb") as f:
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


def cmd_view(opts) -> int:
    import math

    from gm import render, sbw

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
        try:
            if ext == ".sbw":
                w = sbw.parse_file(p)
                meshes = render.level_meshes(w)
                kind = f"level {len(w.sectors)}s/{w.n_objects}o"
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
            data = render.contact_sheet(meshes, opts.size, pool=pool,
                                        zoom=opts.zoom, uv=opts.uv,
                                        wire=opts.wire, cull=opts.cull,
                                        lit=not opts.flat)
        else:
            data = render.render(meshes, opts.size, pool=pool, az=az, el=el,
                                 zoom=opts.zoom, uv=opts.uv, wire=opts.wire,
                                 cull=opts.cull, lit=not opts.flat)
        dt = time.time() - t0
        name = os.path.splitext(os.path.basename(p))[0]
        out = os.path.join(opts.out, name + ".png") if out_is_dir else opts.out
        with open(out, "wb") as f:
            f.write(data)
        tex_used = len({fc.texture for m in meshes for fc in m.faces})
        print(f"{os.path.basename(p)}: {kind}, {n_faces} faces, "
              f"{tex_used} textures, az={az:g} el={el:g}, "
              f"coverage {render.coverage(data):.1%}, {dt:.2f}s -> {out}")
        if opts.ascii:
            print(render.ascii_preview(data, opts.ascii))
    return rc


def cmd_sob(opts) -> int:
    files = sorted(
        os.path.join(opts.src, f) for f in os.listdir(opts.src) if f.lower().endswith(".sob")
    )
    ok = 0
    errs = 0
    for p in files:
        try:
            sob.to_obj_file(p, opts.out)
            ok += 1
        except Exception as exc:  # noqa: BLE001 - report and continue
            errs += 1
            print(f"  FAIL {os.path.basename(p)}: {exc}", file=sys.stderr)
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

    p = sub.add_parser("sounds", help="sounds/SOU.DAT -> WAV (SOU0) / raw payloads (SOU1-2)")
    p.add_argument("game")
    p.add_argument("out")
    p.set_defaults(func=cmd_sounds)

    p = sub.add_parser("sbc", help=".sbc lightmap blobs -> per-record grayscale PNG + contact sheet")
    p.add_argument("paths", nargs="+", help="one or more .sbc files, or directories to scan")
    p.add_argument("out")
    p.add_argument("--rebuild", help="write each .sbc back (re-serialised) into this directory")
    p.set_defaults(func=cmd_sbc)

    p = sub.add_parser("frontend", help="FRONTEND/BMP.DAT -> BMP")
    p.add_argument("game")
    p.add_argument("out")
    p.set_defaults(func=cmd_frontend)

    p = sub.add_parser("sob", help="convert .sob models to OBJ/MTL")
    p.add_argument("src")
    p.add_argument("out")
    p.set_defaults(func=cmd_sob)

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
    p.add_argument("--ascii", type=int, nargs="?", const=78, default=0,
                   help="also print a coarse ASCII preview (optional width)")
    p.add_argument("--palette", help="BMP file to take the palette from")
    p.add_argument("--game", help="installation dir (for textures and the palette)")
    p.set_defaults(func=cmd_view)

    p = sub.add_parser("soa", help="list .soa animation descriptors")
    p.add_argument("paths", nargs="+", help="one or more .soa files or directories")
    p.add_argument("--rebuild", help="write each .soa back (re-serialised) into this directory")
    p.set_defaults(func=cmd_soa)

    p = sub.add_parser("soe", help="parse .soe thing-type definitions (text)")
    p.add_argument("paths", nargs="+", help="one or more .soe files or directories")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="dump every field of the token stream")
    p.add_argument("--rebuild", help="write each .soe back (re-serialised) into this directory")
    p.set_defaults(func=cmd_soe)

    p = sub.add_parser("look", help="analyse LOOK16.TAB / LOOK162.TAB lookup cubes")
    p.add_argument("file")
    p.add_argument("--png", help="write the cube as an 8x8 montage PNG")
    p.add_argument("--palette", help="BMP to colourise the PNG (default: grey ramp)")
    p.add_argument("--neons", help="NEONS.PIN to cross-check against the value set")
    p.add_argument("--scale", type=int, default=1, help="integer upscale for --png")
    p.set_defaults(func=cmd_look)

    p = sub.add_parser("sog", help="parse .sog songs (text playlists)")
    p.add_argument("path", help="a .sog file or the MUSIC directory")
    p.add_argument("--music", help="MUSIC/SOU.IDX to resolve sample names against")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--rebuild", help="write each .sog back (re-serialised) into this directory")
    p.set_defaults(func=cmd_sog)

    p = sub.add_parser("sbv", help="parse .sbv video (header + frame blocks)")
    p.add_argument("file")
    p.add_argument("out", nargs="?", help="output dir for raw frame blocks")
    p.add_argument("--list-only", action="store_true")
    p.add_argument("--png", help="decode frames and write PNGs into this directory")
    p.add_argument("--frames", default="0", help="frames to decode for --png: "
                   "'0', '1,5,9-12' or 'all' (default: 0)")
    p.add_argument("--scale", type=int, default=1, help="integer upscale for --png (default: 1)")
    p.add_argument("--no-raw", action="store_true", help="do not write raw frame blocks to OUT")
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

    opts = ap.parse_args(argv)
    return opts.func(opts)


if __name__ == "__main__":
    raise SystemExit(main())
