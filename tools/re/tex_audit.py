#!/usr/bin/env python3
"""Texture-decode audit for `.sbw` levels (M4: materials / textures / lightmaps).

Question
--------
Our preview render shows magenta where the game shows lit walls, and one of the
open hypotheses was "the yellow/black hazard stripes (``HAZFLR``) are decoded
wrong and come out magenta".  This tool answers the narrower, cheaper question
first: **does the `.sbm` decode itself produce magenta?**

Method
------
1. Parse a level with :mod:`gm.sbw`.
2. Collect texture names from both places the engine uses:

   * the level's texture-name table (``level.tex``, 381 names in ``level0``);
   * the polygon records — engine encoding (``docs`` in ``gm/sbw.py``):
     ``0x00 > 0`` is a **1-based** entry of the texture table, ``< 0`` a
     1-based entry of the effect table, ``0`` = untextured.

3. Resolve every unique name exactly the way ``gm.render.TexturePool`` does:
   ``PICS/SBM.IDX`` first, then ``sprites/SBM.IDX``, case-insensitive retry,
   entry size = next offset in the index (the index carries no sizes).
4. Decode with :mod:`gm.sbm` (header + mip chain, 8-bit palette indices).
5. Colour the indices with two palettes:

   * ``bmp``     — ``gm.palette.installed()`` (what the viewer uses today:
     the first candidate, ``FLAG/naUSA.BMP``);
   * ``gamepal`` — ``FRONTEND/GAMEPAL.BMP`` from ``FRONTEND/BMP.DAT``, the
     palette the engine actually loads into ``0x83d320``
     (``tools/re/init_flow.py`` step 83, ``0x47efe0``).

   Per texture: size, mip count, mean RGB, share of magenta-like texels
   (``r > 200 and b > 200 and g < 80``), share of palette index 0, plus the
   yellow / near-black shares used for the hazard-stripe question.

The two palettes differ in 21 slots (index 0 and the ``NEONS.PIN`` set
``{173,174,175,191,240..255}``): the BMP carrier holds ``(255,0,255)``
placeholders there, the engine palette holds real colours.  Reporting the same
numbers under both separates "decode broke" from "wrong palette carrier".

Usage
-----
    python3 tools/re/tex_audit.py --self-test
    python3 tools/re/tex_audit.py --level level0
    python3 tools/re/tex_audit.py --level level0 level1 --json > audit.json
    python3 tools/re/tex_audit.py --level level0 --dump /var/tmp/texaudit

``--dump`` writes PNGs of every texture whose magenta share reaches
``--magenta-threshold`` (and of the whole ``--watch`` list) for both palettes so
the result can be eyeballed.  Nothing is written into the repository and the
game installation is only ever opened read-only.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import bigfile, bmp, palette as palmod, png, sbm, sbw  # noqa: E402
from gm.sbw import EffectRef, TexRef  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
CACHE_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")

#: Textures named in the M4 task (hazard stripes and the floor/light family).
WATCH = ("HAZFLR", "WRNLIT2", "FLATFLR", "GRATE5", "WHITEFL2")

#: A magenta-like texel: both red and blue saturated, green low.
MAGENTA = (200, 200, 80)          # r > 200, b > 200, g < 80
YELLOW = (180, 150, 90)           # r > 180, g > 150, b < 90
BLACK_MAX = 60                    # r < 60, g < 60, b < 60

PERCENT = 100.0


def default_game() -> str:
    """The installation to audit: ``$GUNMETAL_GAME``, mirror, installed game."""
    env = os.environ.get("GUNMETAL_GAME")
    if env:
        return env
    if os.path.isdir(CACHE_GAME):
        return CACHE_GAME
    return DEFAULT_GAME


# ---------------------------------------------------------------------------
# palette carriers
# ---------------------------------------------------------------------------


def palette_from_bmp(game: str) -> bytes:
    """What ``gm.render`` uses today (``gm.palette.installed``)."""
    return palmod.installed(game)


def palette_from_gamepal(game: str) -> tuple[bytes | None, str]:
    """``FRONTEND/GAMEPAL.BMP`` from the bigfile — the engine's own palette.

    The loader (``0x47efe0``) reads this file into ``0x83d320`` at startup and
    ``0x83d2c4`` points the current-palette pointer at it, so every 8-bit art
    asset, levels included, is indexed into it.
    """
    idx = os.path.join(game, "FRONTEND", "BMP.IDX")
    if not os.path.exists(idx):
        return None, "missing FRONTEND/BMP.IDX"
    try:
        bf = bigfile.BigFile(idx)
    except (OSError, ValueError) as exc:            # pragma: no cover - bad install
        return None, f"unreadable FRONTEND/BMP.IDX: {exc}"
    offs = {e.name: e.offset for e in bf.entries}
    order = sorted(offs.values())
    for key in ("GAMEPAL", "gamepal", "Gamepal"):
        off = offs.get(key)
        if off is None:
            continue
        nxt = order[bisect_after(order, off)]
        size = nxt - off
        with open(bf.dat_path, "rb") as fh:
            fh.seek(off)
            blob = fh.read(size)
        try:
            return bmp.parse(blob).palette, "FRONTEND/BMP.DAT:GAMEPAL"
        except ValueError as exc:                   # pragma: no cover - bad install
            return None, f"GAMEPAL is not a BMP: {exc}"
    return None, "GAMEPAL not in FRONTEND/BMP.IDX"


def bisect_after(order: list[int], off: int) -> int:
    """Index of the first offset > *off* (``len(order)`` when none)."""
    lo, hi = 0, len(order)
    while lo < hi:
        mid = (lo + hi) // 2
        if order[mid] > off:
            hi = mid
        else:
            lo = mid + 1
    return lo


def palette_rgb(pal: bytes, index: int) -> tuple[int, int, int]:
    i = 4 * index
    if i + 3 > len(pal):
        return (0, 0, 0)
    return (pal[i], pal[i + 1], pal[i + 2])


def palette_diff(a: bytes, b: bytes) -> list[int]:
    """Slots whose RGB differs between two 256-entry carriers."""
    n = min(256, len(a) // 4, len(b) // 4)
    return [i for i in range(n) if a[4 * i:4 * i + 3] != b[4 * i:4 * i + 3]]


def palette_holes(pal: bytes) -> list[int]:
    """Slots whose colour is the ``(255,0,255)`` placeholder."""
    return [i for i in range(min(256, len(pal) // 4))
            if palette_rgb(pal, i) == (255, 0, 255)]


# ---------------------------------------------------------------------------
# archive lookup (same rules as gm.render.TexturePool)
# ---------------------------------------------------------------------------


class Entry:
    """One resolved ``SBM.IDX`` entry."""

    __slots__ = ("name", "dat_path", "offset", "size", "kind")

    def __init__(self, name: str, dat_path: str, offset: int, size: int, kind: str):
        self.name = name
        self.dat_path = dat_path
        self.offset = offset
        self.size = size
        self.kind = kind

    def __repr__(self) -> str:                      # pragma: no cover - debug aid
        return f"Entry({self.name!r}, {self.kind}, off={self.offset}, size={self.size})"


class Archive:
    """``PICS`` then ``sprites`` name lookup, byte-for-byte the pool's rules."""

    def __init__(self, game: str, archives: tuple[str, ...] = ("PICS", "sprites"),
                 maps=None):
        self.game = game
        self._archives = archives
        #: ``[(dat_path, {name: offset}, {offset: size}, kind)]``
        self._maps = maps
        self._cache: dict[str, Entry | None] = {}
        self._blobs: dict[str, bytes | None] = {}

    @classmethod
    def from_maps(cls, maps) -> "Archive":
        return cls("", maps=maps)

    def maps(self):
        if self._maps is None:
            self._maps = []
            for sub in self._archives:
                idx = os.path.join(self.game, sub, "SBM.IDX")
                if not os.path.exists(idx):
                    continue
                bf = bigfile.BigFile(idx)
                name2off = {e.name: e.offset for e in bf.entries}
                order = sorted(set(name2off.values()))
                dat_size = os.path.getsize(bf.dat_path)
                size_of = {}
                for k, off in enumerate(order):
                    nxt = order[k + 1] if k + 1 < len(order) else dat_size
                    size_of[off] = nxt - off
                kind = "sprites" if os.path.basename(sub).lower() == "sprites" else "pics"
                self._maps.append((bf.dat_path, name2off, size_of, kind))
        return self._maps

    def resolve(self, name: str) -> Entry | None:
        if not name or name == "none":
            return None
        if name in self._cache:
            return self._cache[name]
        entry = None
        for dat_path, name2off, size_of, kind in self.maps():
            off = name2off.get(name)
            if off is None:                          # case-insensitive retry
                up = name.upper()
                for k, v in name2off.items():
                    if k.upper() == up:
                        off = v
                        break
            if off is None:
                continue
            entry = Entry(name, dat_path, off, size_of[off], kind)
            break
        self._cache[name] = entry
        return entry

    def blob(self, entry: Entry) -> bytes:
        if entry.name in self._blobs:
            return self._blobs[entry.name] or b""
        with open(entry.dat_path, "rb") as fh:
            fh.seek(entry.offset)
            data = fh.read(entry.size)
        self._blobs[entry.name] = data
        return data

    def decode(self, name: str) -> sbm.Sbm | None:
        """Decoded mip chain, or ``None`` when the name is not in the archives."""
        entry = self.resolve(name)
        if entry is None:
            return None
        try:
            return sbm.parse(self.blob(entry))
        except ValueError:
            return None


# ---------------------------------------------------------------------------
# measurements
# ---------------------------------------------------------------------------


def measure(indices: bytes, width: int, height: int, pal: bytes) -> dict:
    """Colour statistics of one mip level."""
    n = width * height
    pix = indices[:n]
    hist = collections.Counter(pix)
    total = max(1, len(pix))
    sum_r = sum_g = sum_b = 0
    magenta = yellow = black = zero = 0
    for v in pix:
        r, g, b = palette_rgb(pal, v)
        sum_r += r
        sum_g += g
        sum_b += b
        if r > MAGENTA[0] and b > MAGENTA[1] and g < MAGENTA[2]:
            magenta += 1
        if r > YELLOW[0] and g > YELLOW[1] and b < YELLOW[2]:
            yellow += 1
        if r < BLACK_MAX and g < BLACK_MAX and b < BLACK_MAX:
            black += 1
    zero = hist.get(0, 0)
    top = []
    for index, count in hist.most_common(4):
        top.append({"index": index, "rgb": list(palette_rgb(pal, index)),
                    "share": round(count / total, 4)})
    return {
        "pixels": total,
        "padded_pixels": n - len(pix),
        "avg_rgb": [round(sum_r / total, 1), round(sum_g / total, 1),
                    round(sum_b / total, 1)],
        "magenta": round(magenta / total, 4),
        "magenta_px": magenta,
        "yellow": round(yellow / total, 4),
        "black": round(black / total, 4),
        "index0": round(zero / total, 4),
        "distinct": len(hist),
        "top_colors": top,
    }


def level_names(level):
    """``(table_names, face_names, counters, kinds)`` for a parsed level.

    *kinds* maps every referenced name to the set of places it came from:
    ``table`` (texture-name table), ``faces`` (polygon texture index > 0) or
    ``effect`` (polygon texture index < 0, i.e. the animation/effect table --
    those names are **not** texture-pool entries).
    """
    table = [t.name for t in level.tex]
    faces: list[str] = []
    info = collections.Counter()
    kinds: dict[str, set[str]] = {}
    for name in table:
        kinds.setdefault(name, set()).add("table")
    for sector in level.sectors:
        for obj in sector.objects:
            for poly in obj.polys:
                raw = poly.texture
                t = raw if raw < 0x8000 else raw - 0x10000      # u16 read as s16
                if t == 0:
                    info["untextured"] += 1
                elif t > 0:
                    if t <= len(level.tex):
                        name = level.tex[t - 1].name
                        faces.append(name)
                        kinds.setdefault(name, set()).add("faces")
                        info["table_refs"] += 1
                    else:
                        info["out_of_range"] += 1
                else:
                    k = -t
                    if k <= len(level.effects):
                        name = level.effects[k - 1].name
                        faces.append(name)
                        kinds.setdefault(name, set()).add("effect")
                        info["effect_refs"] += 1
                    else:
                        info["out_of_range"] += 1
    return table, faces, dict(info), kinds


def audit_parsed(label: str, level, pal: bytes, pal_alt: bytes | None,
                 archive: Archive, watch=WATCH, alt_name: str = "gamepal") -> dict:
    """Per-level numbers; *pal* is the selected palette, *pal_alt* the other."""
    table, faces, info, kinds = level_names(level)
    all_names = list(dict.fromkeys(table + faces))
    order = ("table", "faces", "effect")

    def source_of(name: str) -> str:
        return "+".join(k for k in order if k in kinds.get(name, ()))

    rows = []
    missing = []
    effects_missing = []
    for name in all_names:
        tex = archive.decode(name)
        if tex is None:
            (effects_missing if "effect" in kinds.get(name, ()) else missing).append(name)
            continue
        entry = archive.resolve(name)
        indices = tex.levels_data[0]
        stats = measure(indices, tex.width, tex.height, pal)
        alt = measure(indices, tex.width, tex.height, pal_alt) if pal_alt else None
        row = {
            "name": name,
            "width": tex.width,
            "height": tex.height,
            "mips": len(tex.levels_data),
            "archive": entry.kind if entry else "?",
            "source": source_of(name),
            "size": f"{tex.width}x{tex.height}",
        }
        row.update(stats)
        if alt is not None:
            row[alt_name] = {
                "avg_rgb": alt["avg_rgb"],
                "magenta": alt["magenta"],
                "index0": alt["index0"],
                "yellow": alt["yellow"],
                "black": alt["black"],
            }
        rows.append(row)

    watch_rows = []
    seen = {r["name"].upper() for r in rows}
    for name in watch:
        tex = archive.decode(name)
        if tex is None:
            watch_rows.append({"name": name, "decoded": False})
            continue
        entry = archive.resolve(name)
        stats = measure(tex.levels_data[0], tex.width, tex.height, pal)
        alt = measure(tex.levels_data[0], tex.width, tex.height, pal_alt) if pal_alt else None
        watch_rows.append({
            "name": name,
            "decoded": True,
            "width": tex.width,
            "height": tex.height,
            "mips": len(tex.levels_data),
            "archive": entry.kind if entry else "?",
            "referenced_by_level": name.upper() in seen,
            "size": f"{tex.width}x{tex.height}",
            "avg_rgb": stats["avg_rgb"],
            "yellow": stats["yellow"],
            "black": stats["black"],
            "index0": stats["index0"],
            "magenta": stats["magenta"],
            "top_colors": stats["top_colors"],
            alt_name: None if alt is None else {
                "avg_rgb": alt["avg_rgb"],
                "magenta": alt["magenta"],
                "index0": alt["index0"],
                "yellow": alt["yellow"],
                "black": alt["black"],
            },
        })

    resolved_names = [r["name"] for r in rows]
    return {
        "level": label,
        "polys": (info.get("table_refs", 0) + info.get("effect_refs", 0)
                  + info.get("untextured", 0) + info.get("out_of_range", 0)),
        "poly_refs": info,
        "table_names": len(table),
        "face_refs": len(faces),
        "names_total": len(all_names),
        "names_resolved": len(resolved_names),
        "names_missing": len(missing),
        "missing": missing,
        "effects_missing": effects_missing,
        "textures": rows,
        "watch": watch_rows,
    }


# ---------------------------------------------------------------------------
# presentation
# ---------------------------------------------------------------------------


def pct(x: float) -> str:
    return f"{PERCENT * x:5.1f}%"


def rgb_text(rgb) -> str:
    return ",".join(f"{int(v):3d}" for v in rgb)


def print_level(rep: dict, pal_name: str, alt_name: str | None,
                top: int = 15, threshold: float = 0.02) -> None:
    info = rep["poly_refs"]
    print(f"== {rep['level']} ==")
    print(f"texture table: {rep['table_names']} names; "
          f"face refs: {rep['face_refs']} "
          f"(table {info.get('table_refs', 0)}, effects {info.get('effect_refs', 0)}, "
          f"untextured polys {info.get('untextured', 0)}, "
          f"out-of-range {info.get('out_of_range', 0)})")
    print(f"names: {rep['names_total']} total, {rep['names_resolved']} resolved, "
          f"{rep['names_missing']} missing texture name(s), "
          f"{len(rep['effects_missing'])} effect name(s) not in SBM")
    if rep["missing"]:
        print("missing: " + ", ".join(rep["missing"]))
    if rep["effects_missing"]:
        print("effect names (animation table, not texture-pool entries): "
              + ", ".join(rep["effects_missing"]))

    rows = rep["textures"]
    total_px = sum(r["pixels"] for r in rows)
    mag_px = sum(r["magenta_px"] for r in rows)
    print(f"decoded: {len(rows)} textures, {total_px} level-0 texels, "
          f"magenta-like {mag_px} ({pct(mag_px / max(1, total_px))}) "
          f"[palette={pal_name}]")
    if alt_name:
        alt_px = sum(int(round(r[alt_name]["magenta"] * r["pixels"])) for r in rows)
        print(f"same textures with {alt_name}: magenta-like {alt_px} "
              f"({pct(alt_px / max(1, total_px))})")

    suspect = sorted([r for r in rows if r["magenta"] >= threshold],
                     key=lambda r: (-r["magenta"], r["name"]))
    print(f"suspicious (magenta >= {PERCENT * threshold:.0f}%): "
          f"{len(suspect)} texture(s)")
    head = f"{'name':12s} {'size':9s} {'mip':>3s} {'avgRGB':>11s} " \
           f"{'magenta':>8s} {'idx0':>6s} {'yellow':>7s} {'black':>6s}"
    print(head)
    for r in suspect[:max(0, top)]:
        print(f"{r['name'][:12]:12s} {r['size']:9s} {r['mips']:3d} "
              f"{rgb_text(r['avg_rgb']):>11s} {pct(r['magenta']):>8s} "
              f"{pct(r['index0']):>6s} {pct(r['yellow']):>7s} {pct(r['black']):>6s}")
    if len(suspect) > top:
        print(f"... {len(suspect) - top} more (raise --top)")

    print("watchlist:")
    print(f"{'name':10s} {'size':9s} {'mip':>3s} {'avgRGB':>11s} "
          f"{'yellow':>7s} {'black':>6s} {'idx0':>6s} "
          f"{'mag[' + pal_name + ']':>13s}"
          + (f" {'mag[' + alt_name + ']':>13s}" if alt_name else ""))
    for w in rep["watch"]:
        if not w["decoded"]:
            print(f"{w['name'][:10]:10s} NOT DECODED")
            continue
        alt = f" {pct(w[alt_name]['magenta']):>13s}" if alt_name else ""
        print(f"{w['name'][:10]:10s} {w['size']:9s} {w['mips']:3d} "
              f"{rgb_text(w['avg_rgb']):>11s} {pct(w['yellow']):>7s} "
              f"{pct(w['black']):>6s} {pct(w['index0']):>6s} "
              f"{pct(w['magenta']):>13s}{alt}")
    print()


def verdict(reports: list[dict], holes_bmp: list[int], holes_game: list[int],
            selected_name: str, alt_name: str | None) -> dict:
    """Summary numbers.  *gamepal* (the engine palette) decides the verdict."""
    rows = [r for rep in reports for r in rep["textures"]]
    px = sum(r["pixels"] for r in rows)

    def share(key: str) -> tuple[float, float]:
        """``(share of all level-0 texels, worst texture)`` for one palette."""
        if key == "self":
            return (sum(r["magenta_px"] for r in rows) / max(1, px),
                    max((r["magenta"] for r in rows), default=0.0))
        return (sum(int(round(r[key]["magenta"] * r["pixels"])) for r in rows) / max(1, px),
                max((r[key]["magenta"] for r in rows), default=0.0))

    sel_share, sel_worst = share("self")
    alt_share, alt_worst = share(alt_name) if alt_name else (None, None)
    gp_key = "self" if selected_name == "gamepal" else alt_name
    gp_share, gp_worst = share(gp_key) if gp_key else (None, None)
    named = [r["name"] for r in rows if r["magenta"] >= 0.02]
    worst_any = max(sel_worst, alt_worst or 0.0)
    magenta_at_tex = bool(gp_worst is not None and gp_worst >= 0.02)
    if gp_worst is None:
        cause = "texture decode" if worst_any >= 0.02 else "none"
    elif magenta_at_tex:
        cause = "texture decode"
    elif worst_any >= 0.02:
        cause = "palette carrier (magenta placeholder slots)"
    else:
        cause = "none"
    return {
        "textures": len(rows),
        "texels": px,
        **{"magenta_share_" + selected_name: round(sel_share, 4),
           "worst_texture_" + selected_name: sel_worst},
        **({} if alt_share is None else {
            "magenta_share_" + alt_name: round(alt_share, 4),
            "worst_texture_" + alt_name: alt_worst}),
        "magenta_share_gamepal": None if gp_share is None else round(gp_share, 4),
        "worst_texture_gamepal": gp_worst,
        "textures_over_2pct": named,
        "missing_names": [n for rep in reports for n in rep["missing"]],
        "effects_missing": [n for rep in reports for n in rep["effects_missing"]],
        "palette_holes_bmp": holes_bmp,
        "palette_holes_gamepal": holes_game,
        "magenta_at_texture_level": magenta_at_tex,
        "cause": cause,
    }


# ---------------------------------------------------------------------------
# dump
# ---------------------------------------------------------------------------


def dump_textures(rep: dict, out_dir: str, pal: bytes, pal_alt: bytes | None,
                  pal_name: str, alt_name: str | None, archive: Archive,
                  threshold: float, watch=WATCH) -> list[str]:
    os.makedirs(out_dir, exist_ok=True)
    wanted = [r["name"] for r in rep["textures"] if r["magenta"] >= threshold]
    for name in watch:
        if name not in wanted:
            wanted.append(name)
    written = []
    for name in wanted:
        tex = archive.decode(name)
        if tex is None:
            continue
        base = "".join(c if c.isalnum() or c in "-_." else "_" for c in name)
        for suffix, palette in ((pal_name, pal), (alt_name, pal_alt)):
            if palette is None:
                continue
            p = os.path.join(out_dir, f"{base}.{suffix}.png")
            with open(p, "wb") as fh:
                fh.write(png.indexed(tex.levels_data[0], tex.width, tex.height, palette))
            written.append(p)
    return written


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------


def _synthetic_palette() -> bytes:
    pal = bytearray(1024)
    cols = {0: (0, 0, 0), 1: (255, 0, 255), 2: (235, 195, 75), 3: (7, 7, 7)}
    for i, (r, g, b) in cols.items():
        pal[4 * i:4 * i + 3] = bytes((r, g, b))
    for i in range(4, 256):                          # fill the rest with grey
        pal[4 * i:4 * i + 3] = bytes((i, i, i))
    return bytes(pal)


def _synthetic_archive(tmpdir: str):
    """8x8 hull texture (2 mips) + a 4x4 effect texture, both in one .dat.

    Level-0 texels of ``HULLTEST``: 32 magenta (index 1), 16 yellow (2),
    8 near-black (3), 8 palette index 0.
    """
    pal = _synthetic_palette()
    pix = bytes([1]) * 32 + bytes([2]) * 16 + bytes([3]) * 8 + bytes([0]) * 8
    blob = sbm.encode(8, 8, pix, levels=2)
    eff = sbm.encode(4, 4, bytes([2]) * 16, levels=1)
    dat = os.path.join(tmpdir, "SBM.DAT")
    with open(dat, "wb") as fh:
        fh.write(blob)
        fh.write(eff)
    maps = [(dat, {"HULLTEST": 0, "EFFNAME": len(blob)},
             {0: len(blob), len(blob): len(eff)}, "pics")]
    return Archive.from_maps(maps), pal, pix


def _fake_level():
    """Duck-typed level: 2 table names, 1 effect, 2 polygons."""
    class P:
        def __init__(self, texture):
            self.texture = texture

    class O:
        def __init__(self, polys):
            self.polys = polys

    class S:
        def __init__(self, objs):
            self.objects = objs

    class L:
        pass

    lv = L()
    lv.tex = [TexRef("HULLTEST", 0, 1), TexRef("MISSINGONE", 0, 1)]
    lv.effects = [EffectRef("EFFNAME", 0, 0, [])]
    lv.sectors = [S([O([P(1), P(0), P(0xFFFF)])])]     # texture 1, none, effect -1
    return lv


def self_test(game: str | None) -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool, detail: str = "") -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(("ok    " if cond else "FAIL  ") + label + (f"  [{detail}]" if detail else ""))

    tmp = tempfile.mkdtemp(prefix="texaudit-")
    try:
        archive, pal, pix = _synthetic_archive(tmp)
        level = _fake_level()

        rep = audit_parsed("synthetic", level, pal, None, archive,
                           watch=("HULLTEST", "NOSUCHTEX"))
        check("synthetic texture resolves", rep["names_resolved"] == 2,
              f"{rep['names_resolved']}/{rep['names_total']}")
        check("missing name lands in misses", rep["missing"] == ["MISSINGONE"],
              str(rep["missing"]))
        by_name = {r["name"]: r for r in rep["textures"]}
        check("face texture 1 -> table[0], effect -1 -> effects[0]",
              by_name["HULLTEST"]["source"] == "table+faces"
              and by_name["EFFNAME"]["source"] == "effect",
              f"{by_name['HULLTEST']['source']} / {by_name['EFFNAME']['source']}")
        check("effect name resolves through the same archive",
              by_name["EFFNAME"]["size"] == "4x4", by_name["EFFNAME"]["size"])

        row = by_name["HULLTEST"]
        check("known texture size 8x8", (row["width"], row["height"]) == (8, 8),
              row["size"])
        check("known texture has 2 mips", row["mips"] == 2, str(row["mips"]))
        check("magenta share == 0.50 (32/64)", abs(row["magenta"] - 0.5) < 1e-9,
              f"{row['magenta']}")
        check("yellow share == 0.25 (16/64)", abs(row["yellow"] - 0.25) < 1e-9,
              f"{row['yellow']}")
        check("index-0 share == 0.125 (8/64)", abs(row["index0"] - 0.125) < 1e-9,
              f"{row['index0']}")
        check("near-black share == 0.25 (idx 3 + idx 0)", abs(row["black"] - 0.25) < 1e-9,
              f"{row['black']}")
        want = [round((255 * 32 + 235 * 16 + 7 * 8) / 64, 1),
                round((195 * 16 + 7 * 8) / 64, 1),
                round((255 * 32 + 75 * 16 + 7 * 8) / 64, 1)]
        check("mean RGB matches the palette", row["avg_rgb"] == want,
              f"got {row['avg_rgb']} want {want}")
        check("decode round-trips the indices",
              archive.decode("HULLTEST").levels_data[0] == pix)
        check("empty watch name is not decoded",
              rep["watch"][1]["decoded"] is False, rep["watch"][1]["name"])
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)

    # ---- shipped data, when an installation is reachable -------------------
    idx = os.path.join(game or "", "PICS", "SBM.IDX")
    if not os.path.exists(idx):
        print("skip  shipped-archive checks (no PICS/SBM.IDX)")
        return 1 if failures else 0
    arch = Archive(game)
    pool_pal = palmod.installed(game)
    hull = arch.decode("ALUMNM4")
    check("ALUMNM4 (hull) resolves", hull is not None)
    if hull is not None:
        check("ALUMNM4 is 64x64 with 7 mips",
              (hull.width, hull.height, len(hull.levels_data)) == (64, 64, 7),
              f"{hull.width}x{hull.height}/{len(hull.levels_data)}")
        stats = measure(hull.levels_data[0], hull.width, hull.height, pool_pal)
        check("ALUMNM4 statistics are non-empty",
              stats["distinct"] > 4 and stats["pixels"] == 64 * 64,
              f"distinct={stats['distinct']}")
    haz = arch.decode("HAZFLR")
    check("HAZFLR (hazard stripes) resolves and carries no magenta", haz is not None
          and measure(haz.levels_data[0], haz.width, haz.height, pool_pal)["magenta"] < 0.02)
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def resolve_level_path(game: str, arg: str) -> str:
    if os.path.exists(arg):
        return arg
    for cand in (os.path.join(game, arg), os.path.join(game, arg + ".sbw")):
        if os.path.exists(cand):
            return cand
    raise SystemExit(f"error: level {arg!r} not found under {game!r}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Audit texture decoding for .sbw levels (M4).",
        epilog="example: python3 tools/re/tex_audit.py --level level0")
    ap.add_argument("--game", default=default_game(),
                    help=f"installation directory (default: {default_game()})")
    ap.add_argument("--level", nargs="+", default=["level0"],
                    metavar="NAME", help="level name(s) or .sbw paths (default: level0)")
    ap.add_argument("--palette", choices=("bmp", "gamepal"), default="bmp",
                    help="palette for the printed table (default: bmp = what the "
                         "viewer uses); both are always measured")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--top", type=int, default=15,
                    help="suspicious rows to print (default: 15)")
    ap.add_argument("--magenta-threshold", type=float, default=0.02,
                    help="share above which a texture counts as suspicious (0.02)")
    ap.add_argument("--watch", nargs="+", default=list(WATCH),
                    help="textures to report separately (default: %(default)s)")
    ap.add_argument("--dump", nargs="?", const="/var/tmp/texaudit", metavar="DIR",
                    help="write PNGs of suspicious/watch textures (default: /var/tmp/texaudit)")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic + shipped-data checks, rc=0 on success")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test(args.game)

    game = args.game
    pool_pal = palette_from_bmp(game)
    game_pal, game_pal_src = palette_from_gamepal(game)
    hole_bmp = palette_holes(pool_pal)
    hole_game = palette_holes(game_pal) if game_pal else []

    if args.palette == "gamepal" and game_pal is None:
        raise SystemExit(f"error: {game_pal_src}")
    selected, selected_name = ((game_pal, "gamepal") if args.palette == "gamepal"
                               else (pool_pal, "bmp"))
    alt, alt_name = ((pool_pal, "bmp") if args.palette == "gamepal"
                     else (game_pal, "gamepal"))
    if alt is None:
        alt_name = None

    archive = Archive(game)
    reports = []
    for level_arg in args.level:
        path = resolve_level_path(game, level_arg)
        label = os.path.basename(path).rsplit(".", 1)[0]
        reports.append(audit_parsed(label, sbw.parse_file(path), selected,
                                    alt, archive, watch=tuple(args.watch),
                                    alt_name=alt_name or "alt"))

    ver = verdict(reports, hole_bmp, hole_game, selected_name, alt_name)
    dumped = []
    if args.dump:
        for rep in reports:
            dumped += dump_textures(rep, args.dump, selected, alt, selected_name,
                                    alt_name, archive, args.magenta_threshold,
                                    watch=tuple(args.watch))

    if args.json:
        out = {
            "game": game,
            "palette": {
                "selected": selected_name,
                "gamepal_source": game_pal_src,
                "holes_bmp": hole_bmp,
                "holes_gamepal": hole_game,
                "diff_slots": diff,
            },
            "verdict": ver,
            "levels": reports,
            "dump": dumped,
        }
        json.dump(out, sys.stdout, indent=1, sort_keys=False)
        sys.stdout.write("\n")
        return 0

    print(f"game: {game}")
    print(f"palette: selected={selected_name}; "
          f"bmp holes {len(hole_bmp)} {hole_bmp if len(hole_bmp) <= 24 else '...'}; "
          f"gamepal holes {len(hole_game)} {hole_game if len(hole_game) <= 24 else '...'} "
          f"({game_pal_src})")
    diff = palette_diff(pool_pal, game_pal) if game_pal else []
    if diff:
        print(f"slots that differ between the carriers: {len(diff)} {diff}")
    print()
    for rep in reports:
        print_level(rep, selected_name, alt_name, top=args.top,
                    threshold=args.magenta_threshold)
    print("VERDICT")
    print(f"  textures decoded: {ver['textures']} ({ver['texels']} level-0 texels)")
    print(f"  missing names: {len(ver['missing_names'])}"
          + (f" {ver['missing_names']}" if ver["missing_names"] else ""))
    print(f"  magenta share [{selected_name}]: "
          f"{pct(ver['magenta_share_' + selected_name])}; "
          f"worst texture {pct(ver['worst_texture_' + selected_name])}")
    if alt_name:
        print(f"  magenta share [{alt_name}]: "
              f"{pct(ver['magenta_share_' + alt_name])}; "
              f"worst texture {pct(ver['worst_texture_' + alt_name])}")
    if ver["magenta_share_gamepal"] is not None and "gamepal" not in (selected_name, alt_name):
        print(f"  magenta share [gamepal]: {pct(ver['magenta_share_gamepal'])}; "
              f"worst texture {pct(ver['worst_texture_gamepal'])}")
    print(f"  magenta at texture level: "
          f"{'YES' if ver['magenta_at_texture_level'] else 'NO'}")
    print(f"  cause: {ver['cause']}")
    if dumped:
        print(f"  dumped {len(dumped)} PNG(s) to {args.dump}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
