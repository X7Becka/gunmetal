#!/usr/bin/env python3
"""Semantics of the ``.soe`` fields as they land in ``THING_TYPES``.

The grammar of ``SOBS/railrock.soe`` is already parsed by :mod:`gm.soe`.
This tool answers the *next* question: **what does each field of the compiled
936-byte ``THING_TYPES`` record mean, and where is that proved?**

Evidence comes from ``gundx.exe`` (v1.36):

* loader ``0x47cb6c`` - reads ``thing\\<name>.soe`` with ``fscanf`` formats and
  writes the record array at ``0x776d60`` (stride ``0x3a8``);
* spawn ``0x46d000`` / ``0x46d8b0`` - copies record fields into a thing
  instance (``0x7ac0c8``, stride ``0x528``);
* consumers - collision ``0x46e282``, damage/shields ``0x470420``, expire
  ``0x4703a0``/``0x470c70``, weapons ``0x473b00``, death/remove
  ``0x470290``/``0x475660``, movement ``0x47093c``, sound ``0x472080``.

Usage::

    python3 tools/re/soe_semantics.py                 # field map + railrock
    python3 tools/re/soe_semantics.py --json          # machine readable
    python3 tools/re/soe_semantics.py --soe PATH      # another .soe file
    python3 tools/re/soe_semantics.py --levels        # coverage over 36 .sbt
    python3 tools/re/soe_semantics.py --check         # validate shipped data
    python3 tools/re/soe_semantics.py --exe-check     # verify xref bytes

The game is only read.  ``GUNMETAL_GAME`` overrides the install directory
(default ``/mnt/d/games/Gunmetal 3dfx``).
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod          # noqa: E402
from gm import soe as soemod          # noqa: E402

GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
EXE = os.environ.get("GUNMETAL_EXE", os.path.join(GAME, "gundx.exe"))

RECORD_SIZE = soemod.RECORD_SIZE
INSTANCE_BASE = 0x7AC0C8

#: Loader entry points / blocks (VA).  ``.soe`` token order is fixed by these
#: ``fscanf`` calls; the format strings are quoted in :data:`LOADER_BLOCKS`.
LOADER = {
    "entry": 0x47CB6C,
    "name_copy": 0x47CB7A,
    "version": 0x47CBF9,
    "id_block": 0x47CC3D,
    "v9_extra": 0x47CC94,
    "movement": 0x47CD8D,
    "collision_shape": 0x47CE51,
    "radii": 0x47CEB6,
    "six_doubles": 0x47CFAD,
    "v7_events": 0x47D010,
    "shields": 0x47D0DA,
    "textures": 0x47D195,
    "expire": 0x47D23E,
    "expire_event": 0x47D26A,
    "ten_field_block": 0x47D2F2,
    "falling_fear": 0x47D55E,
    "create_event": 0x47D596,
    "morale_timeout_damaged": 0x47D620,
    "v7_pair": 0x47D799,
    "v10_pair": 0x47D86A,
    "v11_pair": 0x47D8EF,
    "v12_pair": 0x47D974,
    "v13_leash": 0x47D9D0,
    "path": 0x47DA93,
    "weapons": 0x47DBAA,
    "children": 0x47DD4D,
    "count_guard": 0x47E01C,
}

LOADER_BLOCKS = {
    "version": ("%ld", 0x4A6910),
    "id_block": ("%ld %ld %s %ld %ld", 0x4BBB20),
    "six_doubles": ("%lf %lf %lf %lf", 0x4BB9E4),
    "movement_and_shape": ("%s %lf %lf %lf %lf %lf %lf %s", 0x4BBAE0),
    "v7_events": ("%s %ld", 0x4BB9DC),
    "v7_events_old": ("%s %s %ld", 0x4BB9D0),
    "shields4": ("%ld %ld %ld %ld", 0x4B9640),
    "textures": ("%s %s", 0x4BB93C),
    "ten_field_block": ("%s %lf %lf %lf %ld %s %s %ld %ld %ld", 0x4BB868),
    "v7_pair": ("%ld %ld", 0x4B9820),
    "v10_v11_pair": ("%lf %lf", 0x4BB69C),
    "v13_leash": ("%s %lf", 0x4BB694),
    "weapons": ("%s %ld %ld %ld %ld %ld %ld", 0x4BB4B4),
    "children": ("%s %lf %lf %lf %lf %lf %lf %ld", 0x4BB464),
}

#: Name table of the movement resolver ``0x46c840`` (``0x4ba8c8``).
MOVEMENT_TYPES = [
    "none", "plain", "chaos", "wallflowerleft", "wallflowerright", "turret",
    "bounce", "homing", "visualhoming", "path", "pathloop", "pathrandom",
    "pathexpire", "flee", "attack", "visualattack", "follow", "retreat",
    "pushable",
]

#: Names resolved by loader ``0x46c840``; values are exactly ``MOVEMENT_TYPES``.
SHAPE_KINDS = {"none": 0, "sphere": 1, "ellipsoid": 2}


def _game_files(kind: str) -> list[str]:
    return [p for p in sorted(glob.glob(os.path.join(GAME, "*" + kind)))
            if "saved" not in os.path.basename(p).lower()]


def _record_from_sbt(entry) -> bytes:
    """Rebuild one 936-byte record from :class:`gm.sbt.ThingType`."""
    name = entry.name_raw[:16].ljust(16, b"\0")
    params = b"".join(struct.pack("<d", float(v)) for v in entry.params[:49])
    tail = entry.tail[:528].ljust(528, b"\0")
    rec = name + params + tail
    if len(rec) != RECORD_SIZE:
        raise ValueError("bad record size %d" % len(rec))
    return rec


def _read(rec: bytes, fi, index: int = 0):
    if fi.kind == "double":
        off = fi.offset + (fi.stride or fi.size) * index
        return struct.unpack_from("<d", rec, off)[0]
    if fi.kind == "name16":
        return rec[:16].split(b"\0")[0].decode("latin1")
    off = fi.offset + (fi.stride or fi.size) * index
    return struct.unpack_from("<i", rec, off)[0]


def _label_offset(label: str):
    """Return ``(offset, kind, index)`` for a parser label, or ``None``."""
    fi = soemod.label_info(label)
    if fi is None:
        return None
    idx = 0
    if "[" in label:
        idx = int(label.split("[", 1)[1].split("]", 1)[0])
    if fi.name == "collision_kind":
        return fi.offset, "shape", idx
    if fi.name == "radius":
        return fi.offset, "sphere_radius", idx
    if fi.name in ("radius_x", "radius_y", "radius_z"):
        return fi.offset, "double", idx
    if label.startswith("path_point["):
        return fi.offset + 4 * idx, "int", idx
    if label.startswith("weapon["):
        return fi.offset + 4 * idx, "int" if fi.kind == "int" else "double", idx
    if label.startswith("child["):
        return fi.offset + (fi.stride or fi.size) * idx, "double" if fi.kind == "double" else "int", idx
    return fi.offset, fi.kind, idx


#: String fields that resolve to an identifier / index.  For a numeric token the
#: value is stored; for a name the simulator leaves 0 and reports it.
UNRESOLVED_STRINGS = {
    "class_name": "SOB_TABLE index (+0x1a4)",
    "collision_type": "movement index (+0x1b4)",
    "event_a": "event id (+0x1b8)",
    "expire_event": "event id (+0x1ec)",
    "create_event": "event id (+0x1f0)",
    "morale_failure_event": "event id (+0x1bc)",
    "time_out_event": "event id (+0x1c0)",
    "damaged_event": "event id (+0x1f4)",
    "str_c8": "event id (+0x1f8)",
    "str_f0": "event id (+0x1c4)",
    "str_1a4": "sound sample id (+0x1fc)",
    "leash_location": "location id (+0x204)",
}


def build_record(tt, type_name: str | None = None):
    """Simulate the loader for a parsed :class:`gm.soe.ThingType`.

    Returns ``(record, unresolved)`` where ``unresolved`` lists ``(token, role)``
    names that need the executable's symbol tables (model/event/sound names).
    Numeric tokens are stored verbatim, so ``railrock.soe`` (all ``"0"``/``"NONE"``)
    exercises every offset.
    """
    rec = bytearray(RECORD_SIZE)
    name = (type_name or tt.name or "?").encode("latin1")[:15]
    rec[:len(name)] = name
    unresolved = []
    radii = {}

    # first pass: radii, so that the computed +0x30 can be folded
    for f in tt.fields:
        if f.label in ("radius_x", "radius_y", "radius_z", "radius"):
            if isinstance(f.value, (int, float)):
                radii[f.label] = float(f.value)
        if f.label == "collision_shape":
            struct.pack_into("<i", rec, 0x1A0,
                             SHAPE_KINDS.get(str(f.value).lower(), 0))
        pos = _label_offset(f.label)
        if pos is None:
            continue
        off, kind, _idx = pos
        if kind == "shape" or kind == "sphere_radius":
            continue
        if f.kind == "int":
            struct.pack_into("<i", rec, off, int(f.value))
        elif f.kind == "flt":
            struct.pack_into("<d", rec, off, float(f.value))
        elif f.kind == "str":
            text = str(f.value)
            try:
                struct.pack_into("<i", rec, off, int(text, 10))
            except ValueError:
                if f.label in UNRESOLVED_STRINGS or f.label.startswith(
                        ("weapon[", "child[", "path_point[")):
                    unresolved.append((text, UNRESOLVED_STRINGS.get(
                        f.label, "name/id resolved by the executable")))
                # else: legacy token that is not stored (e.g. event_b < v7)

    # sphere radius: loader copies it into +0x30/+0x38/+0x40/+0x48
    if "radius" in radii:
        r = radii["radius"]
        for off in (0x30, 0x38, 0x40, 0x48):
            struct.pack_into("<d", rec, off, r)
    elif "radius_x" in radii:
        for off, key in ((0x38, "radius_x"), (0x40, "radius_y"), (0x48, "radius_z")):
            struct.pack_into("<d", rec, off, radii.get(key, 0.0))
        struct.pack_into("<d", rec, 0x30,
                         max(radii.get("radius_x", 0.0),
                             radii.get("radius_y", 0.0),
                             radii.get("radius_z", 0.0)))
    return bytes(rec), unresolved


def validate_record(rec: bytes, sob_count: int, name: str = "?") -> list[str]:
    """Loader-level invariants for one ``THING_TYPES`` record."""
    errs = []

    def i(off):
        return struct.unpack_from("<i", rec, off)[0]

    move = i(0x1B4)
    if not 0 <= move < len(MOVEMENT_TYPES):
        errs.append("%s: movement_type %d outside 0..%d"
                    % (name, move, len(MOVEMENT_TYPES) - 1))
    kind = i(0x1A0)
    if kind not in SHAPE_KINDS.values():
        errs.append("%s: collision_kind %d outside 0..2" % (name, kind))
    sob = i(0x1A4)
    if not 0 <= sob < max(1, sob_count):
        errs.append("%s: sob_index %d outside SOB_TABLE (%d)"
                    % (name, sob, sob_count))
    for off, label, limit in ((0x218, "weapon_count", soemod.MAX_WEAPONS),
                              (0x2FC, "children_count", soemod.MAX_CHILDREN),
                              (0x320, "path_count", soemod.MAX_PATH_POINTS)):
        v = i(off)
        if not 0 <= v <= limit:
            errs.append("%s: %s %d outside 0..%d" % (name, label, v, limit))
    for off in (0x1D4, 0x1D8, 0x1DC, 0x1E0, 0x1E4, 0x1E8):
        if i(off) < 0:
            errs.append("%s: negative value at +0x%03x (%d)" % (name, off, i(off)))
    for off in (0x208, 0x20C, 0x1FC):
        if i(off) < -2:
            errs.append("%s: suspicious id at +0x%03x (%d)" % (name, off, i(off)))
    return errs


def _map_bounds() -> list[str]:
    errs = []
    for fi in soemod.field_map():
        if fi.offset < 0 or fi.end > RECORD_SIZE:
            errs.append("field %s at +0x%03x..+0x%03x exceeds record"
                        % (fi.name, fi.offset, fi.end))
    # arrays must not overlap (weapon/path/child blocks are adjacent)
    return errs


def describe_railrock(path: str | None):
    if path is None:
        path = os.path.join(GAME, "SOBS", "railrock.soe")
    tt = soemod.parse_file(path)
    rec, unresolved = build_record(tt)
    rows = []
    for fi in soemod.field_map():
        val = _read(rec, fi)
        rows.append({
            "offset": fi.offset, "name": fi.name, "value": val,
            "role": fi.role, "status": fi.status, "soe": fi.soe,
            "evidence": list(fi.evidence), "loader": fi.loader,
        })
    return {
        "path": path,
        "version": tt.version,
        "tokens": len(tt.fields) + tt.extra,
        "fields": len(tt.fields),
        "extra": tt.extra,
        "record_size": len(rec),
        "unresolved": unresolved,
        "rows": rows,
        "record": rec.hex(),
    }


def scan_levels(verbose: bool = False):
    files = _game_files(".sbt")
    stats = {}
    for fi in soemod.field_map():
        stats[(fi.offset, fi.name)] = {"values": set(), "nonzero": 0, "n": 0}
    errors = []
    types = 0
    levels = []
    for path in files:
        try:
            sbt = sbtmod.parse_file(path)
        except (ValueError, OSError) as exc:
            errors.append("%s: %s" % (path, exc))
            continue
        sobs = 0
        for rec in sbt.records_of(20):
            sobs = max(sobs, len(getattr(rec.payload, "entries", []) or []))
        count = 0
        for rec in sbt.records_of(4):
            if not isinstance(rec.payload, sbtmod.ThingTypes):
                continue
            for entry in rec.payload.entries:
                raw = _record_from_sbt(entry)
                types += 1
                count += 1
                errors.extend(validate_record(raw, sobs, entry.name))
                for fi in soemod.field_map():
                    st = stats[(fi.offset, fi.name)]
                    val = _read(raw, fi)
                    if isinstance(val, float):
                        if st["n"] < 200000:
                            st["values"].add(round(val, 6))
                        if val != 0.0:
                            st["nonzero"] += 1
                    else:
                        st["values"].add(val)
                        if val:
                            st["nonzero"] += 1
                    st["n"] += 1
        levels.append({"file": os.path.basename(path), "types": count})
    rows = []
    for fi in soemod.field_map():
        st = stats[(fi.offset, fi.name)]
        vals = sorted(st["values"])
        rows.append({
            "offset": fi.offset, "name": fi.name, "status": fi.status,
            "distinct": len(vals), "min": vals[0] if vals else None,
            "max": vals[-1] if vals else None,
            "nonzero": st["nonzero"], "n": st["n"],
            "examples": vals[:6],
        })
    return {"files": len(files), "types": types, "rows": rows,
            "errors": errors, "levels": levels}


def print_map():
    print("SOE -> THING_TYPES (936 bytes, runtime array 0x776d60, stride 0x3a8)")
    print("status: proven = loader error string or unambiguous consumer; "
          "hypothesis = offset fixed, role inferred")
    print()
    print("off     kind   name             .soe token              status      role")
    for fi in soemod.field_map():
        token = fi.soe or "-"
        print("0x%03X  %-6s %-16s %-22s %-11s %s"
              % (fi.offset, fi.kind, fi.name, token, fi.status, fi.role))
        if fi.loader or fi.evidence:
            print("        loader: %s" % (fi.loader or "-"))
            if fi.evidence:
                print("        xrefs : %s" % ", ".join(fi.evidence))
    counts = {s: sum(1 for fi in soemod.field_map() if fi.status == s)
              for s in ("proven", "hypothesis", "unknown")}
    print()
    print("summary: %d proven, %d hypothesis, %d unknown (total %d fields)"
          % (counts["proven"], counts["hypothesis"], counts["unknown"],
             len(soemod.field_map())))


def print_railrock(info):
    print("railrock.soe: version=%d tokens=%d fields=%d extra=%d record=%d bytes"
          % (info["version"], info["tokens"], info["fields"], info["extra"],
             info["record_size"]))
    if info["unresolved"]:
        print("unresolved names (need exe symbol tables):")
        for text, role in info["unresolved"]:
            print("  %-16r -> %s" % (text, role))
    print()
    for row in info["rows"]:
        print("0x%03X  %-16s = %-16s %-11s %s"
              % (row["offset"], row["name"], row["value"], row["status"], row["role"]))


def print_levels(info):
    print("scanned %d .sbt files, %d THING_TYPES records" %
          (info["files"], info["types"]))
    print("off     name             distinct  min            max            "
          "nonzero     status")
    for row in info["rows"]:
        print("0x%03X  %-16s %8d  %-14s %-14s %7d/%-6d %s"
              % (row["offset"], row["name"], row["distinct"],
                 row["min"], row["max"], row["nonzero"], row["n"], row["status"]))
    constants = [r for r in info["rows"] if r["distinct"] == 1]
    varying = [r for r in info["rows"] if r["distinct"] > 1]
    print()
    print("constant fields (%d): %s" % (len(constants),
          ", ".join("+0x%03x" % r["offset"] for r in constants[:20])))
    print("varying fields (%d): %s" % (len(varying),
          ", ".join("%s(%d)" % (r["name"], r["distinct"]) for r in varying[:16])))
    if info["errors"]:
        print("INVARIANT ERRORS: %d" % len(info["errors"]))
        for err in info["errors"][:20]:
            print("  " + err)
    else:
        print("invariants: OK (movement/collision/sob/counts/ranges)")


def exe_check():
    """Check that the exe really contains the cited strings and record offsets.

    The check is deliberately conservative: it verifies (a) every loader format
    string and movement-type name, and (b) that the absolute record address of
    each mapped field occurs somewhere in the image.  It does not try to guess
    which instruction is the "right" consumer - those xrefs are listed in the
    field map for manual inspection.
    """
    try:
        sys.path.insert(0, os.path.join(ROOT, "tools", "re"))
        import exelib  # noqa: PLC0415
        image = exelib.data()
    except Exception as exc:  # pragma: no cover - optional
        return {"skipped": str(exc)}

    bad_strings = []
    for key, (fmt, va) in LOADER_BLOCKS.items():
        got = exelib.cstr(va)
        if got != fmt:
            bad_strings.append({"block": key, "va": va, "want": fmt, "got": got})

    bad_movement = []
    for i, name in enumerate(MOVEMENT_TYPES):
        ptr_off = exelib.va2off(0x4BA8C8 + 4 * i)
        if ptr_off is None:
            bad_movement.append({"index": i, "error": "table unreadable"})
            continue
        ptr = struct.unpack_from("<I", image, ptr_off)[0]
        got = exelib.cstr(ptr)
        if got != name:
            bad_movement.append({"index": i, "want": name, "got": got})

    referenced, missing = [], []
    for fi in soemod.field_map():
        needle = struct.pack("<I", 0x776D60 + fi.offset)
        (referenced if needle in image else missing).append(fi.name)

    return {
        "format_strings_checked": len(LOADER_BLOCKS),
        "format_string_errors": bad_strings,
        "movement_types_checked": len(MOVEMENT_TYPES),
        "movement_type_errors": bad_movement,
        "record_fields_checked": len(soemod.field_map()),
        "record_fields_referenced": len(referenced),
        "record_fields_without_absolute_ref": missing,
    }


def print_exe_check(info):
    if "skipped" in info:
        print("exe-check skipped: %s" % info["skipped"])
        return
    print("exe-check: %d loader format strings, %d errors; "
          "%d movement names, %d errors; %d/%d record fields have an absolute ref"
          % (info["format_strings_checked"], len(info["format_string_errors"]),
             info["movement_types_checked"], len(info["movement_type_errors"]),
             info["record_fields_referenced"], info["record_fields_checked"]))
    for err in info["format_string_errors"][:10]:
        print("  format %s @0x%x: want %r got %r"
              % (err["block"], err["va"], err["want"], err["got"]))
    for err in info["movement_type_errors"][:10]:
        print("  movement %s" % err)
    if info["record_fields_without_absolute_ref"]:
        print("  no absolute reference (computed or array element): %s"
              % ", ".join(info["record_fields_without_absolute_ref"]))


def main(argv=None) -> int:
    global GAME, EXE
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("--soe", action="append", default=[],
                    help=".soe file(s) to compile/describe (default railrock)")
    ap.add_argument("--levels", action="store_true",
                    help="scan all shipped .sbt THING_TYPES records")
    ap.add_argument("--check", action="store_true",
                    help="validate map bounds and shipped records")
    ap.add_argument("--exe-check", action="store_true",
                    help="verify xref immediates against gundx.exe")
    ap.add_argument("--game", default=GAME, help="Gunmetal install directory")
    opts = ap.parse_args(argv)
    GAME = opts.game
    EXE = os.environ.get("GUNMETAL_EXE", os.path.join(GAME, "gundx.exe"))

    bounds = _map_bounds()
    rail = [describe_railrock(p) for p in opts.soe] or [describe_railrock(None)]
    levels = scan_levels() if (opts.levels or opts.check) else None
    exe_info = exe_check() if opts.exe_check else None

    if opts.json:
        counts = {s: sum(1 for fi in soemod.field_map() if fi.status == s)
                  for s in ("proven", "hypothesis", "unknown")}
        out = {
            "record_size": RECORD_SIZE,
            "instance_base": INSTANCE_BASE,
            "movement_types": MOVEMENT_TYPES,
            "loader": LOADER,
            "loader_blocks": {k: {"format": v[0], "string_va": v[1]}
                              for k, v in LOADER_BLOCKS.items()},
            "fields": [
                {"offset": fi.offset, "size": fi.size, "kind": fi.kind,
                 "name": fi.name, "role": fi.role, "status": fi.status,
                 "loader": fi.loader, "evidence": list(fi.evidence),
                 "soe": fi.soe, "count": fi.count, "stride": fi.stride,
                 "legacy": fi.legacy, "instance": fi.instance}
                for fi in soemod.field_map()
            ],
            "summary": counts,
            "map_bounds_errors": bounds,
            "railrock": rail,
            "levels": levels,
            "exe_check": exe_info,
        }
        print(json.dumps(out, ensure_ascii=False, indent=2))
    else:
        print_map()
        print()
        for info in rail:
            print_railrock(info)
            print()
        if levels:
            print_levels(levels)
        if levels and levels["errors"]:
            print("INVARIANT ERRORS: %d (see --levels)" % len(levels["errors"]))
        if bounds:
            print("MAP BOUNDS ERRORS: %s" % bounds)
        if exe_info is not None:
            print_exe_check(exe_info)

    rc = 0
    if bounds:
        rc = 1
    if opts.check and levels and levels["errors"]:
        rc = 1
    if exe_info and (exe_info.get("format_string_errors") or
                     exe_info.get("movement_type_errors")):
        rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
