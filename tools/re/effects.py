#!/usr/bin/env python3
"""Negative texture indices: the sprite/animation path of the frame pass (M4.2.6).

``docs/TEXUV.md`` §2.4 noted that a polygon with ``texture < 0`` is *not* an
untextured polygon: the engine resolves it through the animation table at
``0x5642c8`` with the frame selector ``0x41de7d``.  This tool closes that path
down to resources:

* which branch the texture setup ``0x9fde70`` takes for a negative index;
* how the .sbw loader fills the table (``0x42aa63``) and therefore what
  ``-k`` means in a shipped level;
* what the resolved frame id indexes (the image table ``[0x9e82c8]``, the
  same one ordinary textures use);
* statistics over the 36 shipped ``.sbw`` (how many polygons, which values);
* which asset each index resolves to (``<name>.SOA`` + the frame textures in
  ``PICS/SBM.IDX`` / ``sprites/SBM.IDX``).

Everything here is read from the shipped game (read-only) plus the byte
anchors below; Ghidra is not needed.  The frame-selector model mirrors
``docs/SOA.md`` §4, the loader ``docs/SOA.md`` §2 and the image table
``docs/SUB.md``.

Corpus: the negative indices live in the 36 ``.sbw`` (polygon fields), so
those are the polygon source; the 36 ``.sbt`` are loaded as well because
``THING_TYPES`` (type 4) / ``SPRITES`` (type 6, ``effect_id`` at ``+0x20``,
same id space) are the only *thing* side references to this table.

Usage::

    python3 tools/re/effects.py --self-test     # anchors + model + corpus
    python3 tools/re/effects.py --summary       # per-level table
    python3 tools/re/effects.py --hist          # index histogram
    python3 tools/re/effects.py --json          # everything
    python3 tools/re/effects.py --summary --level level0
    python3 tools/re/effects.py --summary --mirror   # use ~/.cache mirror

Exit status: 0 on success, 1 on a failed self-test, 2 on a missing corpus.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
EXE = os.environ.get("GUNMETAL_EXE", os.path.join(GAME, "gundx.exe"))

#: Engine ticks per second (``0x4074e0`` sleeps 11 ms, ``0x44db80`` counts 91
#: ticks as one second; see ``docs/SOA.md`` §3).
TICK_HZ = 91

#: Table bases / strides, all from the loader ``0x42aa63`` and player ``0x41de7d``.
ANIM_BASE = 0x5642C8
ANIM_STRIDE = 0x5C
ANIM_COUNT = 0x5670CC          # count2 of the .sbw, u32
IMAGE_TABLE = 0x9E82C8         # image descriptor table, 1-based, stride 0x10
SPRITE_REGISTRY = 0x76ED40     # 0x800 x 16 B: image ptr, kind, loop, name[9]

#: (VA, bytes, what, status) -- each row is checked against the exe verbatim.
ANCHORS = [
    (0x9FDE78, "66 8b 4b 72",
     "setup: handle = word[poly+0x72] (link_sector+1)", "proven"),
    (0x9FDE8E, "83 f9 01 19 c9",
     "cmp handle,1 ; sbb ecx,ecx  -> 0 or -1", "proven"),
    (0x9FDE9D, "83 e1 88",
     "and ecx,0xffffff88  ->  -0x78 when handle < 1", "proven"),
    (0x9FDEFC, "0f bf 44 0b 78",
     "index = movsx word[poly+ecx+0x78] -> +0x00 or overlay +0x78", "proven"),
    (0x9FDF07, "85 c0", "test eax,eax", "proven"),
    (0x9FDF09, "79 14",
     "jns 0x9fdf1f  (negative index -> effect resolver 0x41de7d)", "proven"),
    (0x9FDF12, "8b 53 68",
     "arg2 = poly+0x68 (animation start tick)", "proven"),
    (0x9FDF17, "e8 61 ff a1 ff",
     "call 0x41de7d(index, poly+0x68, [0x9e82d8])", "proven"),
    (0x9FDF1F, "a3 ac 8c 9e 00",
     "[0x9e8cac] = frame id -- drawn as a plain texture id", "proven"),
    (0x41DE83, "83 c8 ff 2b 45 08",
     "rec = 0x5642c8 + (-1 - index) * 0x5c", "proven"),
    (0x41DE89, "6b c0 5c", "imul eax,eax,0x5c (record stride)", "proven"),
    (0x41DE8C, "05 c8 42 56 00", "add eax,0x5642c8 (table base)", "proven"),
    (0x41DEB2, "8b 08", "mode = rec+0x00", "proven"),
    (0x41DECB, "3b 42 04", "cmp t, rec+0x04 (period in ticks)", "proven"),
    (0x41DED9, "0f bf 44 50 0a",
     "mode 0, t>=period: word[rec+0xa+2*declared] = frames[declared-1]",
     "proven"),
    (0x41DEE7, "f7 79 04", "idiv period -> rem = t % period", "proven"),
    (0x41DEFB, "f7 79 04", "idiv period -> k = rem*declared / period", "proven"),
    (0x41DF01, "0f bf 44 42 0c",
     "return word[rec+0x0c+2k] = frames[k]", "proven"),
    (0x41DF18, "e8 33 fd 02 00",
     "modes other than 0/1 -> 0x44dc50, which returns frame 0", "proven"),
    (0x42ABFE, "3b 15 cc 70 56 00",
     "sbw loader: loop over count2 = [0x5670cc]", "proven"),
    (0x42AC54, "81 c1 14 43 56 00",
     "effect name -> rec+0x4c (0x564314)", "proven"),
    (0x42AC71, "05 c8 42 56 00", "mode -> rec+0x00", "proven"),
    (0x42AC97, "81 c2 cc 42 56 00", "period -> rec+0x04", "proven"),
    (0x42ACBE, "81 c1 d0 42 56 00", "declared -> rec+0x08", "proven"),
    (0x42AD0B, "8d 94 48 d4 42 56 00",
     "frames -> rec+0x0c (0x5642d4), u16 each", "proven"),
    (0x41BE0A, "8b 0d ac 8c 9e 00",
     "rasterizer: id = [0x9e8cac] (texture index or effect frame)", "proven"),
    (0x41BE13, "c1 e1 04", "(id-1) << 4 -- 1-based, stride 16", "proven"),
    (0x41BE16, "8b 15 c8 82 9e 00",
     "image table base [0x9e82c8] (same table as ordinary textures)",
     "proven"),
    (0x41C883, "c1 e0 04", "sprite objects: same (id-1)<<4", "proven"),
    (0x41C88C, "8d 54 01 f0",
     "image = [0x9e82c8] + id*16 - 16 = base + (id-1)*16", "proven"),
    (0x423D36, "8a 48 05", "image loader: kind = byte[registry entry+5]", "proven"),
    (0x423D49, "68 b4 8d 4a 00", "kind 0 -> \"pics\\\\%s.sbm\"", "proven"),
    (0x423DB1, "68 cc 8d 4a 00", "kind 1 -> \"sprites\\\\%s.sbm\"", "proven"),
    (0x41B6DD, "e8 6e 27 5e 00",
     "polygon drawer 0x41b6cb calls the texture setup 0x9fde50", "proven"),
    (0x43AFF5, "89 44 11 68",
     "object spawn: poly[i]+0x68 = t, per-poly animation start tick",
     "proven"),
]

#: Totals over the 36 shipped ``.sbw`` (read-only corpus, 2026-10-23).
GOLDEN = {
    "levels": 36,
    "polys": 235069,
    "neg_texture": 6117,
    "neg_overlay": 133,
    "overlay_used": 18,
    "pairs": 262,              # (level, effect name) pairs on the effect path
    "distinct_effects": 48,    # distinct effect names reached by level polygons
    "frame_refs": 1274,        # frame textures referenced by those records
    "hist_texture": {-13: 32, -12: 40, -11: 170, -10: 362, -9: 101, -8: 351,
                     -7: 251, -6: 675, -5: 338, -4: 912, -3: 630, -2: 1042,
                     -1: 1213},
    "hist_effective": {-13: 32, -12: 40, -11: 170, -10: 362, -9: 101, -8: 351,
                       -7: 251, -6: 675, -5: 340, -4: 912, -3: 630, -2: 1050,
                       -1: 1221},
    "modes_declared": {0: 542, 1: 764, 100: 1, 101: 12, 102: 14},
    "modes_reached": {1: 6094, 100: 3, 101: 22, 102: 16},
    "sbt": {"files": 36, "thing_types": 2896, "thing_type_names": 397,
            "sprite_records": 0, "name_overlap": ["frbl", "lasflare"]},
}


# ---------------------------------------------------------------------------
# proven model (no game data needed)
# ---------------------------------------------------------------------------
def s16(value):
    """Unsigned 16-bit field -> signed (the engine uses ``movsx``)."""
    return value - 0x10000 if value >= 0x8000 else value


def index_source(texture, overlay, link_sector):
    """Which polygon field the setup reads (``0x9fde8e``..``0x9fdefc``).

    ``ecx = (link_sector < 1) ? -0x78 : 0`` and the index is
    ``movsx word[poly+ecx+0x78]``: ``poly+0x00`` for a static polygon,
    ``poly+0x78`` (overlay) for one that links to another sector.
    """
    return texture if link_sector < 1 else overlay


def record_offset(index):
    """``-k`` -> byte offset of its record in the animation table."""
    if index >= 0:
        raise ValueError("only negative indices are effect/animation ids")
    return (-1 - int(index)) * ANIM_STRIDE


def effect_for_index(index, effects):
    """``-k`` -> ``effects[k-1]`` (the .sbw effect table, loader order)."""
    if index >= 0:
        raise ValueError("only negative indices are effect/animation ids")
    k = -int(index)
    if k > len(effects):
        raise IndexError("effect id %d outside the table of %d records"
                         % (index, len(effects)))
    return effects[k - 1]


def frame_index(mode, period_ticks, nframes, tick):
    """0-based frame selected by ``0x41de7d`` at ``tick`` engine ticks.

    Proven branches (``docs/SOA.md`` §4):

    * ``mode == 0``: ``tick >= period`` -> **last** frame, otherwise the
      formula; the effect plays once and then holds its last frame.
    * ``mode == 1``: the formula, looping forever.
    * any other mode: ``0x44dc50``, which always returns ``frame[0]``
      (modes 100/101/102 additionally *draw* into that texture).

    The engine does not clamp negative time: ``idiv`` leaves a negative
    remainder and the index runs before the array, so this model refuses it
    instead of pretending to know what is read.
    """
    n = int(nframes)
    if n <= 0:
        raise ValueError("effect record has no frames")
    if mode not in (0, 1):
        return 0
    period = int(period_ticks)
    if period <= 0:
        raise ZeroDivisionError("period is 0 ticks: the engine divides by zero")
    t = int(tick)
    if t < 0:
        raise ValueError("negative time is not modelled (engine reads out of bounds)")
    if mode == 0 and t >= period:
        return n - 1
    return (t % period) * n // period


def read_record(blob, n=0):
    """Decode record ``n`` of a raw animation table (927 bytes on disk).

    Layout proven by the .sbw loader ``0x42aa63`` (see the anchors)::

        +0x00 i32 mode      +0x04 i32 period_ticks   +0x08 i32 declared
        +0x0c u16 frames[declared]                   +0x4c char name[16]

    The .sbw stores the *runtime* record: the period is already converted
    from centiseconds to ticks (``period * 91 / 100``), which is why it can be
    fed to :func:`frame_index` without conversion.
    """
    base = n * ANIM_STRIDE
    if base + ANIM_STRIDE > len(blob):
        raise IndexError("record %d outside the table" % n)
    mode, period, declared = struct.unpack_from("<iii", blob, base)
    if declared < 0 or declared > 0x20:
        raise ValueError("implausible frame count %d" % declared)
    if declared:
        frames = struct.unpack_from("<%dH" % declared, blob, base + 0x0C)
    else:
        frames = ()
    name = blob[base + 0x4C:base + 0x5C].split(b"\0")[0].decode("latin1")
    return {"mode": mode, "period_ticks": period, "declared": declared,
            "frames": frames, "name": name}


# ---------------------------------------------------------------------------
# corpus
# ---------------------------------------------------------------------------
def _import_gm():
    try:
        from gm import sbw  # noqa: F401  (import check only)
    except ImportError as exc:  # pragma: no cover - broken checkout
        raise SystemExit("gm/ is not importable from %s: %s" % (ROOT, exc))
    return sbw


def level_paths(game):
    """``[(name, path)]`` of the shipped ``.sbw`` files, sorted by name."""
    if not os.path.isdir(game):
        return []
    out = []
    for entry in sorted(os.listdir(game)):
        if entry.lower().endswith(".sbw"):
            out.append((entry[:-4], os.path.join(game, entry)))
    return out


def sbt_paths(game):
    """``[(name, path)]`` of the shipped ``.sbt`` files, sorted by name."""
    if not os.path.isdir(game):
        return []
    return [(entry[:-4], os.path.join(game, entry))
            for entry in sorted(os.listdir(game))
            if entry.lower().endswith(".sbt")]


def scan_sbt(game):
    """``THING_TYPES`` / ``SPRITES`` records of the 36 shipped ``.sbt``.

    ``SPRITES`` (type 6) carries ``effect_id`` at ``+0x20`` -- the same id
    space as a polygon texture index (``0x41c7f4`` -> ``0x41de7d``), so this
    is the only place where a *thing* could reference an animation directly.
    In the shipped levels the record is absent, which is why the correlation
    cannot be measured on real data (see docs/EFFECTS.md §3.4).
    """
    from gm import sbt

    files = thing_types = sprites = 0
    tt_names = set()
    ids = collections.Counter()
    for _name, path in sbt_paths(game):
        try:
            table = sbt.parse_file(path)
        except (OSError, ValueError):
            continue
        files += 1
        for record in table.records:
            payload = record.payload
            if record.type == 4:                       # THING_TYPES
                for entry in payload.entries:
                    thing_types += 1
                    tt_names.add(entry.name.lower())
            elif record.type == 6:                     # SPRITES
                sprites += payload.count
                for entry in payload.entries:
                    ids[entry["effect_id"]] += 1
    return {
        "files": files,
        "thing_types": thing_types,
        "thing_type_names": tt_names,
        "sprite_records": sprites,
        "sprite_effect_ids": ids,
    }


def _asset_indexes(game):
    """Lower-case names in ``PICS/SBM.IDX`` and ``sprites/SBM.IDX``.

    Returns ``(pics, sprites)`` sets; empty when the archives are absent (the
    statistics then report "unresolved" instead of failing).
    """
    from gm import bigfile

    out = []
    for sub in ("PICS", "sprites"):
        path = os.path.join(game, sub, "SBM.IDX")
        try:
            archive = bigfile.BigFile(path)
            out.append({e.name.lower() for e in archive.entries})
        except (OSError, ValueError):
            out.append(set())
    return out[0], out[1]


def _soa_index(game):
    """Lower-case ``.soa`` name -> ``(subdir, filename)``."""
    found = {}
    for sub in ("spritani", "anims", "flaganim"):
        directory = os.path.join(game, sub)
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            if entry.lower().endswith(".soa"):
                found.setdefault(entry[:-4].lower(), (sub, entry))
    return found


def _soa_check(path, effect):
    """Compare a ``.soa`` file with the .sbw record (mode/period/frames/names)."""
    from gm import soa

    with open(path, "rb") as fh:
        parsed = soa.parse(fh.read())
    return {
        "file": path,
        "mode": parsed.mode,
        "period_ticks": parsed.period_ticks,
        "frames": parsed.declared_frames,
        "names": [n.lower() for n in parsed.engine_names()],
        "mode_match": parsed.mode == effect["mode"],
        "period_match": parsed.period_ticks == effect["period_ticks"],
        "frames_match": parsed.declared_frames == effect["frame_count"],
    }


def scan_level(name, path, pics=frozenset(), sprites=frozenset(), soa_files=None):
    """Statistics of one ``.sbw``; no writes, no Ghidra."""
    sbw = _import_gm()
    level = sbw.parse_file(path)
    soa_files = soa_files or {}

    hist_texture = collections.Counter()
    hist_effective = collections.Counter()
    used = {}                      # effect name (lower) -> dict
    index_names = collections.defaultdict(collections.Counter)
    neg_texture = neg_overlay = overlay_used = 0
    gated_nidx = 0

    for sector in level.sectors:
        for obj in sector.objects:
            for poly in obj.polys:
                texture = s16(poly.texture)
                overlay = s16(poly.overlay)
                link = poly.handle            # disk slot 0x72 = link_sector+1
                if texture < 0:
                    neg_texture += 1
                    hist_texture[texture] += 1
                if overlay < 0:
                    neg_overlay += 1
                index = index_source(texture, overlay, link)
                if index >= 0:
                    continue
                if overlay < 0 and link >= 1:
                    overlay_used += 1
                # gate of the setup: n_idx > 2 (0x9fde93) -- checked before the
                # index is even read (0x9fdefc).
                if len(poly.indices) <= 2:
                    gated_nidx += 1
                    continue
                hist_effective[index] += 1
                effect = effect_for_index(index, level.effects)
                entry = used.setdefault(effect.name.lower(), {
                    "name": effect.name,
                    "mode": effect.kind,
                    "period_ticks": effect.start,
                    "frames": list(effect.ids),
                    "indices": set(),
                    "polys": 0,
                })
                entry["indices"].add(index)
                entry["polys"] += 1
                index_names[index][effect.name] += 1

    rows = []
    missing = []
    modes_reached = collections.Counter()
    declared_frames = 0
    declared_kinds = collections.Counter()
    declared_missing = []
    bad_frames = []
    for record in level.effects:
        declared_frames += len(record.ids)
        for fid in record.ids:
            if not 1 <= fid <= len(level.tex):
                bad_frames.append((record.name, fid))
                continue
            texture = level.tex[fid - 1]
            declared_kinds[(texture.flag >> 8) & 0xFF] += 1
            low = texture.name.lower()
            if low not in pics and low not in sprites:
                declared_missing.append((record.name, fid, texture.name))
    for key in sorted(used):
        entry = used[key]
        modes_reached[entry["mode"]] += entry["polys"]
        names = []
        for fid in entry["frames"]:
            if 0 < fid <= len(level.tex):
                names.append(level.tex[fid - 1].name)
            else:
                names.append(None)
        kinds = []
        archives = []
        for fid, tex_name in zip(entry["frames"], names):
            kind = 0
            if 0 < fid <= len(level.tex):
                kind = (level.tex[fid - 1].flag >> 8) & 0xFF
            kinds.append(kind)
            low = (tex_name or "").lower()
            if low and low in pics:
                archives.append("pics")
            elif low and low in sprites:
                archives.append("sprites")
            else:
                archives.append(None)
                missing.append((key, fid, tex_name))
        row = {
            "name": entry["name"],
            "mode": entry["mode"],
            "period_ticks": entry["period_ticks"],
            "frame_count": len(entry["frames"]),
            "frame_ids": list(entry["frames"]),
            "frame_names": names,
            "frame_kinds": kinds,
            "frame_archives": archives,
            "indices": sorted(entry["indices"]),
            "polys": entry["polys"],
            "soa": soa_files.get(key),
        }
        rows.append(row)

    return {
        "level": name,
        "file": path,
        "polys": sum(len(o.polys) for s in level.sectors for o in s.objects),
        "effects_declared": len(level.effects),
        "textures_declared": len(level.tex),
        "neg_texture": neg_texture,
        "neg_overlay": neg_overlay,
        "overlay_used": overlay_used,
        "effect_polys": sum(hist_effective.values()),
        "gated_nidx_le_2": gated_nidx,
        "hist_texture": {str(k): v for k, v in sorted(hist_texture.items())},
        "hist_effective": {str(k): v for k, v in sorted(hist_effective.items())},
        "index_names": {str(k): dict(v) for k, v in index_names.items()},
        "modes_declared": {str(k): v for k, v in sorted(
            collections.Counter(e.kind for e in level.effects).items())},
        "modes_reached": {str(k): v for k, v in sorted(modes_reached.items())},
        "declared_frame_ids": declared_frames,
        "declared_frame_kinds": {str(k): v for k, v in sorted(declared_kinds.items())},
        "declared_missing": declared_missing,
        "bad_frame_ids": bad_frames,
        "declared_effects": sorted({e.name.lower() for e in level.effects}),
        "effects": rows,
        "missing_assets": missing,
    }


def scan(game, levels=None, verify_soa=True):
    """Scan the corpus; returns the full model (also the ``--json`` payload)."""
    paths = level_paths(game)
    if not paths:
        raise FileNotFoundError("no .sbw levels under %r" % game)
    if levels:
        wanted = {name.lower() for name in levels}
        paths = [(n, p) for n, p in paths if n.lower() in wanted]
        if not paths:
            raise FileNotFoundError("no .sbw level named %s under %r"
                                    % ("/".join(levels), game))

    pics, sprites = _asset_indexes(game)
    soa_files = _soa_index(game)

    per_level = []
    hist_texture = collections.Counter()
    hist_effective = collections.Counter()
    by_effect = collections.Counter()
    index_names = collections.defaultdict(collections.Counter)
    modes_declared = collections.Counter()
    modes_reached = collections.Counter()
    declared_names = set()
    pairs = 0
    frame_refs = 0
    unique_frames = set()
    names = set()
    for name, path in paths:
        row = scan_level(name, path, pics, sprites, soa_files)
        if verify_soa:
            for effect in row["effects"]:
                where = soa_files.get(effect["name"].lower())
                effect["soa"] = where
                if where:
                    full = os.path.join(game, where[0], where[1])
                    try:
                        check = _soa_check(full, effect)
                    except (OSError, ValueError) as exc:
                        effect["soa_check"] = {"error": str(exc)}
                    else:
                        textured = [n.lower() if n else None
                                    for n in effect["frame_names"]]
                        check["names_match"] = check["names"] == textured
                        effect["soa_check"] = check
        hist_texture.update({int(k): v for k, v in row["hist_texture"].items()})
        hist_effective.update({int(k): v for k, v in row["hist_effective"].items()})
        for key, counts in row["index_names"].items():
            index_names[int(key)].update(counts)
        modes_declared.update({int(k): v for k, v in row["modes_declared"].items()})
        modes_reached.update({int(k): v for k, v in row["modes_reached"].items()})
        declared_names.update(row["declared_effects"])
        pairs += len(row["effects"])
        for effect in row["effects"]:
            by_effect[(effect["name"], effect["mode"])] += effect["polys"]
            names.add(effect["name"].lower())
            frame_refs += effect["frame_count"]
            unique_frames.update(n.lower() for n in effect["frame_names"] if n)
        per_level.append(row)

    sbt_info = scan_sbt(game)
    return {
        "game": game,
        "exe": EXE,
        "model": {
            "anim_table": "0x%06x" % ANIM_BASE,
            "anim_stride": "0x%02x" % ANIM_STRIDE,
            "anim_count": "0x%06x" % ANIM_COUNT,
            "image_table": "0x%06x" % IMAGE_TABLE,
            "sprite_registry": "0x%06x" % SPRITE_REGISTRY,
            "tick_hz": TICK_HZ,
            "index_rule": "index = link_sector < 1 ? texture(+0x00) : overlay(+0x78)",
            "record": "mode +0x00, period_ticks +0x04, declared +0x08, "
                      "frames +0x0c, name +0x4c",
            "frame_rule": "mode 0/1: frames[(t%period)*declared/period]; "
                          "mode 0 clamps to frames[declared-1] at t>=period; "
                          "other modes return frames[0]",
        },
        "anchors": [{"address": "0x%06x" % va, "bytes": raw, "what": what,
                     "status": status} for va, raw, what, status in ANCHORS],
        "totals": {
            "levels": len(per_level),
            "polys": sum(r["polys"] for r in per_level),
            "neg_texture": sum(r["neg_texture"] for r in per_level),
            "neg_overlay": sum(r["neg_overlay"] for r in per_level),
            "overlay_used": sum(r["overlay_used"] for r in per_level),
            "effect_polys": sum(r["effect_polys"] for r in per_level),
            "pairs": pairs,
            "distinct_effects": len(names),
            "declared_effect_names": len(declared_names),
            "sbt": {
                "files": sbt_info["files"],
                "thing_types": sbt_info["thing_types"],
                "thing_type_names": len(sbt_info["thing_type_names"]),
                "sprite_records": sbt_info["sprite_records"],
                "sprite_effect_ids": sbt_info["sprite_effect_ids"],
                "name_overlap": sorted(declared_names
                                       & sbt_info["thing_type_names"]),
            },
            "frame_refs": frame_refs,
            "unique_frame_textures": len(unique_frames),
            "missing_assets": sum(len(r["missing_assets"]) for r in per_level),
            "modes_declared": modes_declared,
            "modes_reached": modes_reached,
            "declared_frame_ids": sum(r["declared_frame_ids"] for r in per_level),
            "declared_frame_kinds": sum((collections.Counter(
                {int(k): v for k, v in r["declared_frame_kinds"].items()})
                for r in per_level), collections.Counter()),
            "declared_missing": sum(len(r["declared_missing"]) for r in per_level),
            "bad_frame_ids": sum(len(r["bad_frame_ids"]) for r in per_level),
            "hist_texture": hist_texture,
            "hist_effective": hist_effective,
            "index_names": index_names,
            "by_effect": by_effect,
        },
        "levels": per_level,
    }


# ---------------------------------------------------------------------------
# presentation
# ---------------------------------------------------------------------------
def _hist_lines(hist, names=None):
    names = names or {}
    total = sum(hist.values()) or 1
    out = ["| index | polygons | share | most frequent effect |",
           "|---:|---:|---:|---|"]
    for key in sorted(hist):
        value = hist[key]
        common = ""
        if names.get(key):
            (top, count), = collections.Counter(names[key]).most_common(1)
            common = "%s (%d)" % (top, count)
        out.append("| %d | %d | %.2f %% | %s |"
                   % (key, value, 100.0 * value / total, common))
    out.append("| | **%d** | 100 %% | |" % total)
    return out


def print_hist(model):
    print("histogram over %d levels (effective index, both sources)"
          % model["totals"]["levels"])
    print("\n".join(_hist_lines(model["totals"]["hist_effective"],
                                model["totals"]["index_names"])))
    print()
    print("raw `poly+0x00` only (index -> polygons):")
    print("\n".join(_hist_lines(model["totals"]["hist_texture"])))


def print_summary(model):
    totals = model["totals"]
    print("effect path (negative texture index), %d levels, %d polygons"
          % (totals["levels"], totals["polys"]))
    print("  polys with texture<0 : %6d  (raw poly+0x00)"
          % totals["neg_texture"])
    print("  polys with overlay<0 : %6d  (raw poly+0x78)"
          % totals["neg_overlay"])
    print("  of those overlay used: %6d  (link_sector != 0 -> overlay is the index)"
          % totals["overlay_used"])
    print("  on the effect path   : %6d" % totals["effect_polys"])
    print("  distinct ids         : %6d" % len(totals["hist_effective"]))
    print("  (level, effect) pairs: %6d" % totals["pairs"])
    print("  distinct effects     : %6d" % totals["distinct_effects"])
    print("  frame textures       : %6d references, %d unique, %d unresolved"
          % (totals["frame_refs"], totals["unique_frame_textures"],
             totals["missing_assets"]))
    head = lambda counter: ", ".join("%d:%d" % (k, v)
                                     for k, v in sorted(counter.items()))
    print("  modes declared       : %s" % head(totals["modes_declared"]))
    print("  modes reached by polys: %s" % head(totals["modes_reached"]))
    sbt = totals["sbt"]
    print("  .sbt records         : THING_TYPES %d (%d names), SPRITES %d"
          % (sbt["thing_types"], sbt["thing_type_names"], sbt["sprite_records"]))
    print("  name overlap         : %s"
          % (", ".join(sbt["name_overlap"]) or "-"))
    print()
    header = ("level", "polys", "tex<0", "ov<0", "used", "ids", "effects")
    print("%-14s %7s %6s %5s %5s %4s %7s" % header)
    for row in model["levels"]:
        print("%-14s %7d %6d %5d %5d %4d %7d"
              % (row["level"], row["polys"], row["neg_texture"],
                 row["neg_overlay"], row["overlay_used"],
                 len(row["hist_effective"]), len(row["effects"])))
    print()
    print("top effects by polygon count:")
    for (name, mode), count in totals["by_effect"].most_common(12):
        print("  %-10s mode=%-4d %5d" % (name, mode, count))
    print()
    print("index -> effect (per level, first occurrence):")
    seen = set()
    for row in model["levels"]:
        for effect in row["effects"]:
            key = (effect["name"], effect["mode"], effect["period_ticks"])
            if key in seen:
                continue
            seen.add(key)
            archives = collections.Counter(a or "MISSING"
                                           for a in effect["frame_archives"])
            where = effect["soa"]
            soa_name = "%s/%s" % where if where else "MISSING"
            print("  %-10s mode=%-4d period=%-4d frames=%-2d %-40s %s"
                  % (effect["name"], effect["mode"], effect["period_ticks"],
                     effect["frame_count"], soa_name,
                     ", ".join("%s %d" % (k, v) for k, v in archives.items())))


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def verify_anchors(exe=EXE):
    """Byte check of :data:`ANCHORS` against the shipped exe."""
    sys.path.insert(0, HERE)
    import exelib

    missing = []
    if not os.path.exists(exe):
        return None, missing
    data = exelib.data()
    for va, raw, _what, _status in ANCHORS:
        off = exelib.va2off(va)
        want = bytes.fromhex(raw)
        if off is None or data[off:off + len(want)] != want:
            missing.append(va)
    return data, missing


def self_test(verbose=True):
    """Anchors + frame model + corpus invariants; True when everything holds."""
    results = []

    def check(name, condition, detail=""):
        results.append((name, bool(condition), detail))
        if verbose:
            print("%s  %-58s %s" % ("ok  " if condition else "FAIL", name, detail))
        return condition

    # -- model -------------------------------------------------------------
    check("-k maps to record k-1", record_offset(-1) == 0
          and record_offset(-2) == ANIM_STRIDE
          and record_offset(-13) == 12 * ANIM_STRIDE)
    check("non-negative index is not an effect id",
          _raises(ValueError, record_offset, 0) and _raises(ValueError, record_offset, 3))
    fake = [{"name": "A"}, {"name": "B"}]
    check("-1 -> effects[0], -2 -> effects[1]",
          effect_for_index(-1, fake)["name"] == "A"
          and effect_for_index(-2, fake)["name"] == "B")
    check("out-of-range effect id is refused", _raises(IndexError, effect_for_index, -3, fake))

    check("mode 1 loops: (t%p)*n/p",
          [frame_index(1, 10, 5, t) for t in range(11)] == [0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 0])
    check("mode 0 holds the last frame at t == period", frame_index(0, 10, 5, 10) == 4)
    check("mode 0 holds it forever", frame_index(0, 10, 5, 10 ** 6) == 4)
    check("mode 0 == mode 1 inside the first period",
          all(frame_index(0, 65, 3, t) == frame_index(1, 65, 3, t) for t in range(65)))
    check("BRIEF: 3 frames over 65 ticks change at 22 and 44",
          [frame_index(1, 65, 3, t) for t in (0, 21, 22, 43, 44, 64)] == [0, 0, 1, 1, 2, 2])
    check("special modes 100/101/102 always return frame 0",
          all(frame_index(m, 0, 1, 12345) == 0 for m in (100, 101, 102)))
    check("negative time is refused", _raises(ValueError, frame_index, 1, 10, 5, -1))
    check("zero period is refused", _raises(ZeroDivisionError, frame_index, 1, 0, 5, 0))
    check("empty frame list is refused", _raises(ValueError, frame_index, 1, 10, 0, 0))

    # record decoding against a hand-built 0x5c record
    blob = bytearray(ANIM_STRIDE)
    struct.pack_into("<iii", blob, 0, 1, 65, 3)
    struct.pack_into("<3H", blob, 0x0C, 43, 44, 45)
    blob[0x4C:0x4C + 6] = b"BRIEF\0"
    rec = read_record(bytes(blob))
    check("record: mode/period/declared at +0x00/+0x04/+0x08",
          (rec["mode"], rec["period_ticks"], rec["declared"]) == (1, 65, 3))
    check("record: frames at +0x0c", rec["frames"] == (43, 44, 45))
    check("record: name at +0x4c", rec["name"] == "BRIEF")

    # -- anchors -----------------------------------------------------------
    data, missing = verify_anchors()
    if data is None:
        check("exe present for anchor check", False, "no %s" % EXE)
    else:
        check("anchors byte-identical to %s (%d rows)" % (os.path.basename(EXE),
                                                          len(ANCHORS)),
              not missing,
              "" if not missing else "mismatch at %s"
              % ", ".join("0x%06x" % va for va in missing))

    # -- corpus ------------------------------------------------------------
    paths = level_paths(GAME)
    if not paths:
        check("36 shipped .sbw present", False, "no levels under %r" % GAME)
        return _report(results)
    model = scan(GAME)
    totals = model["totals"]
    check("36 levels scanned", totals["levels"] == GOLDEN["levels"],
          "%d" % totals["levels"])
    check("235 069 polygons", totals["polys"] == GOLDEN["polys"],
          "%d" % totals["polys"])
    check("6 117 polygons with texture<0", totals["neg_texture"] == GOLDEN["neg_texture"],
          "%d" % totals["neg_texture"])
    check("133 polygons with overlay<0", totals["neg_overlay"] == GOLDEN["neg_overlay"],
          "%d" % totals["neg_overlay"])
    check("overlay really used as index 18 times",
          totals["overlay_used"] == GOLDEN["overlay_used"], "%d" % totals["overlay_used"])
    check("effective-index histogram matches the golden",
          dict(totals["hist_effective"]) == GOLDEN["hist_effective"])
    check("raw texture histogram matches the golden",
          dict(totals["hist_texture"]) == GOLDEN["hist_texture"])
    check("262 (level, effect) pairs", totals["pairs"] == GOLDEN["pairs"],
          "%d" % totals["pairs"])
    check("48 distinct effects", totals["distinct_effects"] == GOLDEN["distinct_effects"],
          "%d" % totals["distinct_effects"])
    check("every effect index stays inside the level's table",
          all(-min(int(k) for k in row["hist_effective"]) <= row["effects_declared"]
              for row in model["levels"]))
    check("no polygon with n_idx <= 2 on the effect path",
          all(row["gated_nidx_le_2"] == 0 for row in model["levels"]))
    check("every frame id is 1..textures_declared", _frames_in_range(model))
    check("every referenced frame texture resolves to an SBM archive",
          totals["missing_assets"] == 0, "%d unresolved" % totals["missing_assets"])
    frame_refs = sum(e["frame_count"] for r in model["levels"] for e in r["effects"])
    check("1 274 frame-id references", frame_refs == GOLDEN["frame_refs"],
          "%d" % frame_refs)
    check("212 unique frame textures",
          totals["unique_frame_textures"] == 212,
          "%d" % totals["unique_frame_textures"])
    check("declared effect modes match the golden",
          dict(totals["modes_declared"]) == GOLDEN["modes_declared"],
          str(dict(sorted(totals["modes_declared"].items()))))
    check("polygons reach only looping/special modes (never a one-shot mode 0)",
          dict(totals["modes_reached"]) == GOLDEN["modes_reached"],
          str(dict(sorted(totals["modes_reached"].items()))))
    check("all 10 762 frame ids of the declared records are 1..textures_declared",
          totals["declared_frame_ids"] == 10762 and totals["bad_frame_ids"] == 0,
          "%d ids, %d bad" % (totals["declared_frame_ids"], totals["bad_frame_ids"]))
    check("all declared frame textures resolve (4242 PICS / 6520 sprites)",
          dict(totals["declared_frame_kinds"]) == {0: 4242, 1: 6520}
          and totals["declared_missing"] == 0,
          "kinds %s, %d missing" % (dict(sorted(totals["declared_frame_kinds"].items())),
                                    totals["declared_missing"]))
    mismatched = []
    checked = 0
    for row in model["levels"]:
        for effect in row["effects"]:
            chk = effect.get("soa_check") or {}
            if not effect.get("soa"):
                mismatched.append((row["level"], effect["name"], "no .soa"))
                continue
            checked += 1
            if not (chk.get("mode_match") and chk.get("period_match")
                    and chk.get("frames_match") and chk.get("names_match")):
                mismatched.append((row["level"], effect["name"], chk))
    check("each effect has a .soa with the same mode/period/frames/names",
          not mismatched, "%d checked, problems: %s" % (checked, mismatched[:3]))

    sbt = totals["sbt"]
    golden_sbt = GOLDEN["sbt"]
    check("36 .sbt parsed: 2 896 THING_TYPES, 0 SPRITES records",
          (sbt["files"], sbt["thing_types"], sbt["sprite_records"])
          == (golden_sbt["files"], golden_sbt["thing_types"],
              golden_sbt["sprite_records"]),
          "files %d, thing types %d, sprites %d"
          % (sbt["files"], sbt["thing_types"], sbt["sprite_records"]))
    check("397 distinct THING_TYPES names; only frbl/lasflare match effects",
          sbt["thing_type_names"] == golden_sbt["thing_type_names"]
          and sbt["name_overlap"] == golden_sbt["name_overlap"],
          "%d names, overlap %s" % (sbt["thing_type_names"],
                                    sbt["name_overlap"]))
    return _report(results)


def _frames_in_range(model):
    for row in model["levels"]:
        for effect in row["effects"]:
            for fid in effect["frame_ids"]:
                if not 1 <= fid <= row["textures_declared"]:
                    return False
    return True


def _raises(exc, func, *args):
    try:
        func(*args)
    except exc:
        return True
    except Exception:
        return False
    return False


def _report(results):
    bad = [name for name, ok, _ in results if not ok]
    print("-" * 78)
    print("self-test: %d/%d ok" % (len(results) - len(bad), len(results)))
    for name in bad:
        print("  failed: %s" % name)
    return not bad


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser(
        description="negative texture indices = sprite/animation frames (M4.2.6)")
    parser.add_argument("--game", default=GAME, help="game install (read-only)")
    parser.add_argument("--mirror", action="store_true",
                        help="use ~/.cache/gunmetal-re/game (drvfs is slow)")
    parser.add_argument("--level", action="append", default=None,
                        help="restrict to one level (repeatable)")
    parser.add_argument("--json", action="store_true", help="print the model as JSON")
    parser.add_argument("--hist", action="store_true", help="index histogram")
    parser.add_argument("--summary", action="store_true", help="per-level table")
    parser.add_argument("--self-test", action="store_true",
                        help="anchors + frame model + corpus invariants")
    opts = parser.parse_args(argv)

    if opts.mirror:
        sys.path.insert(0, HERE)
        import gmcache

        opts.game = gmcache.ensure_mirror(opts.game)

    if opts.self_test:
        return 0 if self_test() else 1

    try:
        model = scan(opts.game, opts.level)
    except FileNotFoundError as exc:
        print("effects: %s" % exc, file=sys.stderr)
        return 2

    if opts.json:
        print(json.dumps(_jsonable(model), indent=2, ensure_ascii=False))
        return 0
    if opts.hist:
        print_hist(model)
        return 0
    print_summary(model)
    return 0


def _jsonable(value):
    if isinstance(value, dict):
        return {str(k): _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if isinstance(value, set):
        return sorted(_jsonable(v) for v in value)
    return value


if __name__ == "__main__":
    raise SystemExit(main())
