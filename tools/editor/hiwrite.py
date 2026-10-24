#!/usr/bin/env python3
"""Writer/editor for the Gunmetal ``GUNMETAL.HI`` best-time table (``M11.5``,
slice 11).

The file is a fixed table of **9 records of 0x40 bytes** (576 bytes in the
shipped installation)::

    record (0x40 = 64 bytes):  u32 seconds (little-endian);  char name[60]

The name is a C string in a 60-byte field: ASCII, NUL-terminated, the rest
zero.  The model here is exactly the documented one (``docs/CAMPAIGN_FLOW.md``
§6, ``docs/HI_SCORES.md``, ``engine/src/ui/records.*``,
``engine/src/game/campaign_state.{h,cpp}``, oracle
``tools/re/campaign_flow.py:parse_hi/encode_hi``)::

    raw = b"".join(u32le(seconds) + name + b"\\0" * (60 - len(name)))

Re-serialising the shipped file gives it back byte for byte (**9/9 records,
576/576 bytes**): every name in it is NUL-terminated and every byte after the
terminator is zero.  A file with garbage after the NUL cannot survive this
model (the original compares and prints the name as a C string, so those bytes
are invisible to the game too) -- such a file is reported, not "normalised".

The tool only *edits*: it replaces the 4 time bytes or the 60 name bytes of
**one existing record** and never changes the file size, the record count or a
byte outside that record.  It does not append records (the game appends one
0x40 record on an improvement, ``0x45c904``) and does not create the file.

Usage::

    python3 tools/editor/hiwrite.py --roundtrip            # game/*.hi, recursive
    python3 tools/editor/hiwrite.py --roundtrip FILE
    python3 tools/editor/hiwrite.py --audit                # the shipped table
    python3 tools/editor/hiwrite.py --audit FILE
    python3 tools/editor/hiwrite.py --list                 # slot / time / name
    python3 tools/editor/hiwrite.py --check FILE [--against ORIG]
    python3 tools/editor/hiwrite.py --set-time 2 --seconds 5 --out OUT
    python3 tools/editor/hiwrite.py --set-name 2 --name "KELLER CORE" --out OUT
    python3 tools/editor/hiwrite.py --self-test

Slots are **0-based** (0..8), as in ``gunmetal --best-times`` and in the
history of ``docs/CAMPAIGN_STATE.md`` (``record=2 18->5``).

Exit codes: ``0`` success, ``1`` verification failed (roundtrip/check/self-test
negative), ``2`` bad input or usage (message on stderr), ``3`` internal error.

Refusals (code 2): a file that is not 576 bytes, a time outside
``0..0xFFFFFFFF``, a name that is empty / not ASCII / has a control byte / is
longer than 59 bytes (the 60-byte field has to keep its NUL), ``--out`` inside
the game installation, its regression mirror or either repository root, and
``--out`` equal to the input.  Nothing is ever written into the installation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools", "re"))

try:                                                # the project's .hi oracle
    import campaign_flow as hioracle                # noqa: E402
    ORACLE_ERROR = ""
    ORACLE_PATH = hioracle.__file__
except Exception as _exc:                           # noqa: BLE001
    hioracle = None                                 # type: ignore[assignment]
    ORACLE_ERROR = "%s: %s" % (type(_exc).__name__, _exc)
    ORACLE_PATH = ""

try:                                                # gm/small.py: raw-preserving
    from gm import small as smallmod                # noqa: E402
    TOOLKIT_ERROR = ""
except Exception as _exc:                           # noqa: BLE001
    smallmod = None                                 # type: ignore[assignment]
    TOOLKIT_ERROR = "%s: %s" % (type(_exc).__name__, _exc)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")
#: the game installation in use (``--game`` overrides it)
GAME = DEFAULT_GAME

#: the file name the game opens (``0x4be60c``); the shipped file is lower-case
HI_FILE = "GUNMETAL.HI"
#: one record: ``u32 seconds; char name[60]`` (``docs/CAMPAIGN_FLOW.md`` §6)
RECORD_BYTES = 0x40
NAME_BYTES = 60
#: the shipped table: 576 bytes = 9 records (data, 9/9)
SHIPPED_RECORDS = 9
SHIPPED_BYTES = RECORD_BYTES * SHIPPED_RECORDS
#: the time is a u32 on disk; both project readers take it as ``i32``
U32_MAX = 0xFFFFFFFF
INT32_MAX = 0x7FFFFFFF
#: the longest name that leaves the 60-byte field its terminator
MAX_NAME_BYTES = NAME_BYTES - 1
#: temp root (WSL: ``/tmp`` is tmpfs, keep files tiny; the task asks for /tmp)
TMP_BASE = "/tmp/opencode"


class WriteError(Exception):
    """Bad input or an impossible edit; the message is meant for the user."""


# ---------------------------------------------------------------------------
# small helpers


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fnv1a64(data: bytes) -> int:
    """FNV-1a 64, the fingerprint used in ``docs/CAMPAIGN_STATE.md`` §4."""
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as exc:
        raise WriteError("cannot read %s: %s" % (path, exc.strerror or exc))


def write_bytes(path: str, data: bytes) -> None:
    """Write atomically (``path.tmp`` + ``os.replace``)."""
    parent = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(parent, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except OSError as exc:
        raise WriteError("cannot write %s: %s" % (path, exc.strerror or exc))


def _inside(path: str, root: str) -> bool:
    p = os.path.realpath(os.path.abspath(path))
    r = os.path.realpath(os.path.abspath(root))
    return p == r or p.startswith(r + os.sep)


def source_repo() -> str:
    """The main repository (a git worktree shares ``.git`` with it)."""
    try:
        out = subprocess.run(
            ["git", "-C", ROOT, "rev-parse", "--path-format=absolute",
             "--git-common-dir"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, timeout=10).stdout.strip()
        if out:
            return os.path.dirname(out)
    except (OSError, subprocess.SubprocessError):
        pass
    return "/home/nooverload/gunmetal-re"


MAIN_REPO = source_repo()


def guard_roots() -> tuple:
    """The reference installation, its mirror and both repository roots."""
    return (GAME, DEFAULT_GAME, MIRROR_GAME, ROOT, MAIN_REPO)


def guard_out(out: str, src: str | None = None) -> str:
    """Refuse an output path inside the game / mirror / repository."""
    absolute = os.path.abspath(out)
    for root in guard_roots():
        if _inside(absolute, root):
            raise WriteError(
                "refusing to write %s: it is inside %s (read-only: the game, "
                "its mirror and the repository)" % (out, root))
    if src and os.path.realpath(absolute) == os.path.realpath(src):
        raise WriteError("refusing to overwrite the input file %s" % src)
    return absolute


def esc(text: str) -> str:
    """A printable, byte-faithful rendering of a latin1 name."""
    out = []
    for ch in text:
        code = ord(ch)
        if 0x20 <= code < 0x7F:
            out.append(ch)
        elif code == 0x09:
            out.append("\\t")
        elif code < 0x100:
            out.append("\\x%02x" % code)
        else:
            out.append("\\u%04x" % code)
    return "".join(out)


def diff_stats(a: bytes, b: bytes) -> dict:
    """Positional byte diff: changed positions, first offset, length delta."""
    n = min(len(a), len(b))
    first = None
    changed = 0
    for i in range(n):
        if a[i] != b[i]:
            changed += 1
            if first is None:
                first = i
    if len(a) != len(b):
        changed += abs(len(a) - len(b))
        if first is None:
            first = n
    return {"changed": changed, "first_diff": first,
            "delta": len(b) - len(a)}


def resolve_hi(game: str) -> str:
    """``<game>/GUNMETAL.HI``, case-insensitively (the shipped file is
    ``gunmetal.hi``; ext4 is case-sensitive, drvfs is not)."""
    direct = os.path.join(game, HI_FILE)
    if os.path.exists(direct):
        return direct
    try:
        for name in sorted(os.listdir(game)):
            if name.lower() == HI_FILE.lower():
                return os.path.join(game, name)
    except OSError:
        pass
    return direct


def default_file() -> str:
    return resolve_hi(GAME)


# ---------------------------------------------------------------------------
# the record model (``docs/CAMPAIGN_FLOW.md`` §6)


def parse(raw: bytes, where: str = "") -> list:
    """Decode whole 0x40 records; a partial tail is refused, not rounded."""
    if len(raw) % RECORD_BYTES:
        raise WriteError("%s is %d bytes, not a multiple of %d (one record)"
                         % (where or "<bytes>", len(raw), RECORD_BYTES))
    records = []
    for slot, off in enumerate(range(0, len(raw), RECORD_BYTES)):
        rec = raw[off:off + RECORD_BYTES]
        seconds = struct.unpack_from("<I", rec, 0)[0]
        field = rec[4:]
        nul = field.find(b"\x00")
        terminated = nul >= 0
        name_bytes = field[:nul] if terminated else field
        tail = field[nul + 1:] if terminated else b""
        records.append({
            "slot": slot,
            "offset": off,
            "seconds": seconds,
            "raw_seconds": rec[:4],
            "name_bytes": name_bytes,
            "name": name_bytes.decode("latin1"),
            "terminated": terminated,
            "padding": NAME_BYTES - len(name_bytes),
            "tail_nonzero": sum(1 for byte in tail if byte),
            "ascii": all(0x20 <= byte < 0x7F for byte in name_bytes),
            "empty": not name_bytes,
        })
    return records


def to_bytes(records) -> bytes:
    """Serialise records: ``u32le`` seconds, name, zero padding to 60 bytes."""
    out = bytearray()
    for rec in records:
        name = rec.get("name_bytes")
        if name is None:
            name = rec["name"].encode("latin1")
        if len(name) > NAME_BYTES:
            raise WriteError("name %r is %d bytes, the field is %d"
                             % (name, len(name), NAME_BYTES))
        seconds = int(rec["seconds"])
        if not 0 <= seconds <= U32_MAX:
            raise WriteError("time %d is outside 0..0x%08X" % (seconds, U32_MAX))
        out += struct.pack("<I", seconds)
        out += name + b"\x00" * (NAME_BYTES - len(name))
    return bytes(out)


def analyse(raw: bytes, where: str = "") -> dict:
    records = parse(raw, where)
    return {"raw": raw, "bytes": len(raw), "records": records}


def load(path: str) -> dict:
    """Read one file and require the shipped 9-record shape (576 bytes)."""
    raw = read_bytes(path)
    if len(raw) != SHIPPED_BYTES:
        raise WriteError(
            "%s is %d bytes, expected %d (the shipped table: %d records of "
            "0x%02x); this tool edits the shipped table only, the game "
            "appends records (0x45c904)"
            % (path, len(raw), SHIPPED_BYTES, SHIPPED_RECORDS, RECORD_BYTES))
    doc = analyse(raw, path)
    doc["path"] = path
    return doc


def fold(name: str) -> str:
    """ASCII fold of the name comparison ``0x416850``."""
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in name)


def best_by_name(records) -> dict:
    """Minimum per name, case-insensitively, first spelling wins (``0x45c8d6``).

    The C++ reader (``Records::best_by_name``) folds ASCII; the oracle
    ``campaign_flow.best_by_name`` groups by the exact string.  Both give the
    same 7 names on the shipped file (it has no case twins).
    """
    out = {}
    for rec in records:
        key = fold(rec["name"])
        if key not in out:
            out[key] = {"name": rec["name"], "seconds": rec["seconds"],
                        "entries": 1}
        else:
            out[key]["entries"] += 1
            if rec["seconds"] < out[key]["seconds"]:
                out[key]["seconds"] = rec["seconds"]
    return out


def to_i32(value: int) -> int:
    """The same bytes read as ``i32`` -- what both project readers do."""
    return value - 0x100000000 if value > INT32_MAX else value


def oracle_view(records) -> list:
    """Records in the oracle's shape (``campaign_flow`` uses ``struct <i``)."""
    return [{"seconds": to_i32(rec["seconds"]), "name": rec["name"]}
            for rec in records]


def signed_best(records) -> dict:
    """Minimum per folded name over the *i32* values (the readers' view)."""
    out = {}
    for rec in records:
        key = fold(rec["name"])
        value = to_i32(rec["seconds"])
        out[key] = value if key not in out else min(out[key], value)
    return out


def time_text(seconds: int) -> str:
    """The proven ``"%d : %d%d"`` (``0x483f00``); zero is ``UNKNOWN``."""
    if seconds <= 0:
        return "UNKNOWN"
    return "%d : %d%d" % (seconds // 60, (seconds % 60) // 10, seconds % 10)


def doc_stats(doc: dict) -> dict:
    records = doc["records"]
    filled = [r for r in records if not r["empty"]]
    empty = [r for r in records if r["empty"]]
    times = [r["seconds"] for r in records]
    counts = {}
    for rec in records:
        if rec["empty"]:
            continue
        key = fold(rec["name"])
        counts[key] = counts.get(key, 0) + 1
    dups = {rec["name"]: counts[fold(rec["name"])]
            for rec in records
            if not rec["empty"] and counts[fold(rec["name"])] > 1}
    lengths = [len(r["name_bytes"]) for r in filled]
    return {
        "records": len(records),
        "bytes": len(doc["raw"]),
        "filled": len(filled),
        "empty": len(empty),
        "empty_slots": [r["slot"] for r in empty],
        "times": times,
        "time_min": min(times) if times else None,
        "time_max": max(times) if times else None,
        "distinct_times": len(set(times)),
        "distinct_names": len(counts),
        "duplicates": dups,
        "duplicate_rows": sum(n - 1 for n in counts.values() if n > 1),
        "name_len_min": min(lengths) if lengths else 0,
        "name_len_max": max(lengths) if lengths else 0,
        "padding": sum(r["padding"] for r in records),
        "ascii": sum(1 for r in records if r["ascii"]),
        "terminated": sum(1 for r in records if r["terminated"]),
        "clean_tail": sum(1 for r in records if r["tail_nonzero"] == 0),
    }


# ---------------------------------------------------------------------------
# --check


def doc_problems(doc: dict, where: str) -> dict:
    """Structural verdict: problems (refuse) vs notes/warnings (data)."""
    problems, warnings = [], []
    records = doc["records"]
    stats = doc_stats(doc)
    if len(doc["raw"]) != SHIPPED_BYTES:
        problems.append("%d bytes, the shipped table is %d (%d records of "
                        "0x%02x)" % (len(doc["raw"]), SHIPPED_BYTES,
                                     SHIPPED_RECORDS, RECORD_BYTES))
    if len(records) != SHIPPED_RECORDS:
        problems.append("%d record(s), expected %d"
                        % (len(records), SHIPPED_RECORDS))
    no_nul = [r for r in records if not r["terminated"]]
    if no_nul:
        problems.append(
            "%d record(s) have no NUL in the 60-byte name field (first: slot "
            "%d %r): the game reads the name as a C string and would run into "
            "the next record" % (len(no_nul), no_nul[0]["slot"],
                                 esc(no_nul[0]["name"])))
    bad_ascii = [r for r in records if not r["ascii"]]
    if bad_ascii:
        problems.append(
            "%d record(s) have a name that is not printable ASCII (first: "
            "slot %d %r)" % (len(bad_ascii), bad_ascii[0]["slot"],
                             esc(bad_ascii[0]["name"])))
    dirty = [r for r in records if r["tail_nonzero"]]
    if dirty:
        warnings.append(
            "%d record(s) carry %d non-zero byte(s) after the name's NUL "
            "(first: slot %d): the model writes zeros there, so a rewrite "
            "would drop them -- they are invisible to the game, which prints "
            "the name as a C string" % (len(dirty),
                                        sum(r["tail_nonzero"] for r in dirty),
                                        dirty[0]["slot"]))
    big = [r for r in records if r["seconds"] > INT32_MAX]
    if big:
        warnings.append(
            "%d record(s) store a time above 0x%08X (first: slot %d = 0x%08X): "
            "both readers take the field as i32, see a negative time and "
            "refuse the file (Records::load); the game cannot produce one "
            "(clock ticks / 91)"
            % (len(big), INT32_MAX, big[0]["slot"], big[0]["seconds"]))
    if stats["duplicates"]:
        warnings.append(
            "%d duplicate row(s): %s -- legal, the game appends and keeps the "
            "minimum per name (0x45c8d6)"
            % (stats["duplicate_rows"],
               ", ".join("%s=%d" % (esc(n), c)
                         for n, c in sorted(stats["duplicates"].items()))))
    if stats["empty"]:
        warnings.append("%d empty slot(s): %s (an all-zero name field)"
                        % (stats["empty"], stats["empty_slots"]))
    spaced = [r for r in records
              if r["name"] and r["name"] != r["name"].strip(" ")]
    if spaced:
        warnings.append("%d name(s) with leading/trailing spaces (compared "
                        "bytewise, so they are part of the name)"
                        % len(spaced))
    return {"problems": problems, "warnings": warnings, "stats": stats}


def _slot_diff(before: bytes, after: bytes) -> dict | None:
    """First record whose bytes differ (same-size tables)."""
    for slot in range(min(len(before), len(after)) // RECORD_BYTES):
        off = slot * RECORD_BYTES
        a = before[off:off + RECORD_BYTES]
        b = after[off:off + RECORD_BYTES]
        if a != b:
            old = parse(a, "before")[0]
            new = parse(b, "after")[0]
            return {"slot": slot, "before": {"seconds": old["seconds"],
                                             "name": old["name"]},
                    "after": {"seconds": new["seconds"],
                              "name": new["name"]},
                    "changed_bytes": diff_stats(a, b)["changed"]}
    return None


def check_file(path: str, against: str | None = None) -> dict:
    doc = load(path)
    verdict = doc_problems(doc, path)
    rep = {
        "tool": "hiwrite", "mode": "check", "file": os.path.abspath(path),
        "bytes": len(doc["raw"]), "records": len(doc["records"]),
        "sha256": sha256(doc["raw"]), "fnv1a64": "0x%016x" % fnv1a64(doc["raw"]),
        "stats": verdict["stats"],
        "problems": verdict["problems"], "warnings": verdict["warnings"],
    }
    rep["structure_ok"] = not verdict["problems"]
    rep["ok"] = rep["structure_ok"]
    if against:
        other = load(against)
        cmp = diff_stats(doc["raw"], other["raw"])
        rep["against"] = os.path.abspath(against)
        rep["against_bytes"] = len(other["raw"])
        rep["against_sha256"] = sha256(other["raw"])
        rep["identical"] = doc["raw"] == other["raw"]
        rep["changed_bytes"] = cmp["changed"]
        rep["first_diff"] = cmp["first_diff"]
        rep["delta"] = cmp["delta"]
        rep["records_before"] = len(other["records"])
        rep["first_changed_slot"] = _slot_diff(other["raw"], doc["raw"])
    return rep


def print_check(rep) -> None:
    print("check: %s" % rep["file"])
    st = rep["stats"]
    print("  %d bytes, %d records, sha256=%s"
          % (rep["bytes"], rep["records"], rep["sha256"][:16]))
    print("  fnv1a64=%s  times=%d..%d  filled=%d empty=%d  distinct-names=%d"
          % (rep["fnv1a64"], st["time_min"] if st["time_min"] is not None else -1,
             st["time_max"] if st["time_max"] is not None else -1,
             st["filled"], st["empty"], st["distinct_names"]))
    print("  ascii=%d/%d  terminated=%d/%d  clean-tail=%d/%d"
          % (st["ascii"], st["records"], st["terminated"], st["records"],
             st["clean_tail"], st["records"]))
    if rep.get("against"):
        print("  against %s (%d bytes): %s"
              % (rep["against"], rep["against_bytes"],
                 "IDENTICAL" if rep["identical"] else "differs"))
        if not rep["identical"]:
            print("    changed %d byte(s) (positional), first diff at %s, "
                  "delta %+d, records %d -> %d"
                  % (rep["changed_bytes"],
                     "offset %d" % rep["first_diff"]
                     if rep["first_diff"] is not None else "n/a",
                     rep["delta"], rep["records_before"], rep["records"]))
            sd = rep["first_changed_slot"]
            if sd:
                print("    first changed slot %d: %d %s -> %d %s "
                      "(%d byte(s) inside the record)"
                      % (sd["slot"], sd["before"]["seconds"],
                         esc(sd["before"]["name"]), sd["after"]["seconds"],
                         esc(sd["after"]["name"]), sd["changed_bytes"]))
    for warning in rep["warnings"]:
        print("  note: %s" % warning)
    for problem in rep["problems"]:
        print("  problem: %s" % problem)
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --list


def list_records(path: str) -> dict:
    doc = load(path)
    rows = []
    seen = {}
    for rec in doc["records"]:
        key = fold(rec["name"]) if not rec["empty"] else ""
        duplicate = bool(key) and key in seen
        if key:
            seen[key] = seen.get(key, 0) + 1
        rows.append({
            "slot": rec["slot"], "offset": rec["offset"],
            "seconds": rec["seconds"], "time": time_text(rec["seconds"]),
            "name": rec["name"], "name_bytes": len(rec["name_bytes"]),
            "empty": rec["empty"], "padding": rec["padding"],
            "duplicate": duplicate,
        })
    return {"tool": "hiwrite", "mode": "list", "file": os.path.abspath(path),
            "bytes": len(doc["raw"]), "records": len(rows), "rows": rows}


def print_list(rep) -> None:
    print("list: %s (%d records, %d bytes)"
          % (rep["file"], rep["records"], rep["bytes"]))
    print("  %4s %6s %7s  %-11s %s"
          % ("slot", "offset", "seconds", "time", "name"))
    for row in rep["rows"]:
        name = esc(row["name"]) if row["name"] else "<empty>"
        print("  %4d %6d %7d  %-11s %s%s"
              % (row["slot"], row["offset"], row["seconds"], row["time"], name,
                 "  (duplicate)" if row["duplicate"] else ""))


# ---------------------------------------------------------------------------
# --audit


def audit_file(path: str = "") -> dict:
    path = path or default_file()
    doc = load(path)
    verdict = doc_problems(doc, path)
    stats = verdict["stats"]
    best = best_by_name(doc["records"])
    rep = {
        "tool": "hiwrite", "mode": "audit", "file": os.path.abspath(path),
        "bytes": len(doc["raw"]), "records": len(doc["records"]),
        "sha256": sha256(doc["raw"]), "fnv1a64": "0x%016x" % fnv1a64(doc["raw"]),
        "times": stats["times"],
        "names": [rec["name"] for rec in doc["records"]],
        "stats": stats,
        "best": [{"name": entry["name"], "seconds": entry["seconds"],
                  "entries": entry["entries"]}
                 for entry in sorted(best.values(),
                                     key=lambda e: (e["seconds"],
                                                    fold(e["name"])))],
        "problems": verdict["problems"], "warnings": verdict["warnings"],
        "time_text": [time_text(rec["seconds"]) for rec in doc["records"]],
    }
    rep["ok"] = not verdict["problems"]
    return rep


def print_audit(rep) -> None:
    print("audit: %s" % rep["file"])
    print("  %d bytes, %d records, sha256=%s"
          % (rep["bytes"], rep["records"], rep["sha256"][:16]))
    print("  fnv1a64=%s" % rep["fnv1a64"])
    print("  %4s %7s  %-11s %s" % ("slot", "seconds", "time", "name"))
    st = rep["stats"]
    for slot, (seconds, text, name) in enumerate(
            zip(rep["times"], rep["time_text"], rep["names"])):
        print("  %4d %7d  %-11s %s"
              % (slot, seconds, text, esc(name) if name else "<empty>"))
    print("  times=%s" % ",".join(str(s) for s in rep["times"]))
    print("  names=%s" % " | ".join(esc(n) if n else "<empty>"
                                    for n in rep["names"]))
    print("  filled=%d empty=%d empty-slots=%s"
          % (st["filled"], st["empty"], st["empty_slots"] or "none"))
    print("  time-range=%s..%s distinct-times=%d name-len=%d..%d padding=%d"
          % (st["time_min"], st["time_max"], st["distinct_times"],
             st["name_len_min"], st["name_len_max"], st["padding"]))
    print("  distinct-names=%d duplicate-rows=%d %s"
          % (st["distinct_names"], st["duplicate_rows"],
             ", ".join("%s=%d" % (esc(n), c)
                       for n, c in sorted(st["duplicates"].items())) or ""))
    print("  ascii=%d/%d terminated=%d/%d clean-tail=%d/%d"
          % (st["ascii"], st["records"], st["terminated"], st["records"],
             st["clean_tail"], st["records"]))
    print("  best: %s" % ", ".join("%s=%d(%d)" % (esc(entry["name"]),
                                                  entry["seconds"],
                                                  entry["entries"])
                                   for entry in rep["best"]))
    for warning in rep["warnings"]:
        print("  note: %s" % warning)
    for problem in rep["problems"]:
        print("  problem: %s" % problem)
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --set-time / --set-name (one record, every other byte preserved)


def parse_u32(text: str) -> int:
    try:
        value = int(str(text), 0)
    except ValueError:
        raise WriteError("--seconds %r is not an integer (decimal or 0x...)"
                         % text)
    if not 0 <= value <= U32_MAX:
        raise WriteError("--seconds %d is outside 0..0x%08X (the field is a "
                         "u32; the readers use i32)" % (value, U32_MAX))
    return value


def validate_name(text: str) -> dict:
    """Check a new level name; returns warnings, raises on an impossible one."""
    if not text:
        raise WriteError("--name must not be empty (an empty name is an empty "
                         "slot, and empty names are not written here)")
    try:
        data = text.encode("ascii")
    except UnicodeEncodeError:
        raise WriteError("--name %r is not ASCII: the field is a C string of "
                         "8-bit characters, and the project's names are ASCII"
                         % text)
    if b"\x00" in data:
        raise WriteError("--name must not contain a NUL byte")
    control = [b for b in data if b < 0x20 or b == 0x7F]
    if control:
        raise WriteError("--name %r contains a control byte 0x%02x; names are "
                         "printable ASCII" % (text, control[0]))
    if len(data) > MAX_NAME_BYTES:
        raise WriteError("--name is %d bytes; at most %d fit the 60-byte field "
                         "with its NUL terminator" % (len(data),
                                                      MAX_NAME_BYTES))
    warnings = []
    if text != text.strip(" "):
        warnings.append("name %r has leading/trailing spaces: the comparison "
                        "0x416850 is bytewise, so they are part of the name"
                        % text)
    if len(data) > 40:
        warnings.append("name is %d bytes: it still fits the field, but no "
                        "shipped name is longer than 13" % len(data))
    return {"bytes": data, "warnings": warnings}


def set_record(path: str, slot: int, out: str, seconds=None, name=None) -> dict:
    """Replace the time or the name of record *slot*; every other byte stays."""
    out = guard_out(out, path)
    doc = load(path)
    records = doc["records"]
    if not 0 <= slot < len(records):
        raise WriteError("--slot %d is out of range: %s has %d record(s), "
                         "slots 0..%d"
                         % (slot, path, len(records), len(records) - 1))
    old = records[slot]
    warnings = []
    if seconds is not None:
        mode = "set-time"
        if name is not None:
            raise WriteError("--set-time takes --seconds, not --name")
        new_seconds = parse_u32(str(seconds))
        new_name_bytes = old["name_bytes"]
        if new_seconds > INT32_MAX:
            warnings.append(
                "time 0x%08X is above 0x%08X: both readers take the field as "
                "i32 and will refuse the file (Records::load)"
                % (new_seconds, INT32_MAX))
    else:
        mode = "set-name"
        if name is None:
            raise WriteError("--set-name needs --name TEXT")
        checked = validate_name(name)
        warnings.extend(checked["warnings"])
        new_name_bytes = checked["bytes"]
        new_seconds = old["seconds"]
        if not old["empty"] and fold(old["name"]) != fold(checked["bytes"]
                                                          .decode("latin1")):
            warnings.append("this rewrites a level name (%r -> %r); the other "
                            "rows of the old name are left as they are"
                            % (old["name"], name))

    new_record = struct.pack("<I", new_seconds) + new_name_bytes + \
        b"\x00" * (NAME_BYTES - len(new_name_bytes))
    if len(new_record) != RECORD_BYTES:
        raise WriteError("internal: the new record is %d bytes"
                         % len(new_record))
    off = slot * RECORD_BYTES
    new_raw = doc["raw"][:off] + new_record + doc["raw"][off + RECORD_BYTES:]

    # proof 1: the table shape and every byte outside the record are untouched
    if len(new_raw) != len(doc["raw"]):
        raise WriteError("internal: the edit changed the file size")
    if new_raw[:off] != doc["raw"][:off] or \
            new_raw[off + RECORD_BYTES:] != doc["raw"][off + RECORD_BYTES:]:
        raise WriteError("internal: the edit changed a neighbouring record")
    outside = diff_stats(doc["raw"], new_raw)
    changed_in_record = diff_stats(doc["raw"][off:off + RECORD_BYTES],
                                   new_raw[off:off + RECORD_BYTES])["changed"]
    if outside["changed"] != changed_in_record:
        raise WriteError("internal: %d byte(s) changed outside slot %d"
                         % (outside["changed"] - changed_in_record, slot))

    # proof 2: the parser sees exactly the expected table
    redo = analyse(new_raw, out)
    expect_seconds = new_seconds
    expect_name = new_name_bytes.decode("latin1")
    got = redo["records"][slot]
    if got["seconds"] != expect_seconds or got["name"] != expect_name:
        raise WriteError("internal: re-reading gives slot %d = %d %r"
                         % (slot, got["seconds"], got["name"]))
    for i, rec in enumerate(records):
        if i == slot:
            continue
        mirror = redo["records"][i]
        if (rec["seconds"], rec["name"]) != (mirror["seconds"], mirror["name"]):
            raise WriteError("internal: slot %d changed" % i)
    old_filled = sum(1 for r in records if not r["empty"])
    new_filled = sum(1 for r in redo["records"] if not r["empty"])
    if new_filled != old_filled + (1 if old["empty"] else 0):
        raise WriteError("internal: filled slots %d -> %d" % (old_filled,
                                                              new_filled))

    # proof 3: the project's oracle decodes the same table
    oracle_ok = None
    if hioracle is not None:
        want = oracle_view(redo["records"])
        if hioracle.parse_hi(new_raw) != want:
            raise WriteError("internal: campaign_flow.parse_hi disagrees")
        if hioracle.encode_hi(want) != new_raw:
            raise WriteError("internal: campaign_flow.encode_hi differs from "
                             "the output")
        ours = signed_best(redo["records"])
        theirs = {fold(k): v for k, v in hioracle.best_by_name(want).items()}
        if ours != theirs:
            raise WriteError("internal: the minimum per name differs from the "
                             "oracle")
        oracle_ok = True

    # proof 4: the output passes --check
    verdict = doc_problems(redo, out)
    if verdict["problems"]:
        raise WriteError("internal: the edited file does not pass --check: %s"
                         % "; ".join(verdict["problems"]))

    rep = {
        "tool": "hiwrite", "mode": mode, "file": os.path.abspath(path),
        "out": out, "slot": slot, "records": len(records),
        "before": {"seconds": old["seconds"], "name": old["name"]},
        "after": {"seconds": new_seconds, "name": expect_name},
        "bytes_in": len(doc["raw"]), "bytes_out": len(new_raw),
        "delta": len(new_raw) - len(doc["raw"]),
        "changed_bytes": outside["changed"],
        "changed_in_record": changed_in_record,
        "first_diff": outside["first_diff"],
        "sha256_in": sha256(doc["raw"]), "sha256_out": sha256(new_raw),
        "oracle_ok": oracle_ok,
        "check_ok": True,
        "check_warnings": verdict["warnings"],
        "warnings": warnings,
    }
    write_bytes(out, new_raw)
    if sha256(read_bytes(out)) != rep["sha256_out"]:
        raise WriteError("internal: %s on disk does not match the report" % out)
    return rep


def print_set(rep) -> None:
    print("edit: %s -> %s (slot %d/%d)"
          % (rep["file"], rep["out"], rep["slot"], rep["records"]))
    print("  time: %d -> %d; name: %r -> %r"
          % (rep["before"]["seconds"], rep["after"]["seconds"],
             rep["before"]["name"], rep["after"]["name"]))
    print("  bytes: %d -> %d (%+d); changed %d byte(s), first at offset %s "
          "(%d inside the record)"
          % (rep["bytes_in"], rep["bytes_out"], rep["delta"],
             rep["changed_bytes"],
             rep["first_diff"] if rep["first_diff"] is not None else "n/a",
             rep["changed_in_record"]))
    print("  sha256: %s -> %s"
          % (rep["sha256_in"][:16], rep["sha256_out"][:16]))
    print("  oracle campaign_flow: %s; --check: %s"
          % ("agrees" if rep["oracle_ok"] else "skipped",
             "ok" if rep["check_ok"] else "FAILED"))
    for warning in rep["warnings"]:
        print("  warning: %s" % warning)


# ---------------------------------------------------------------------------
# --roundtrip


def hi_files(path: str = "") -> list:
    """``*.hi`` under *path* (recursive, case-insensitive), or *path* itself."""
    if path and os.path.isfile(path):
        return [path]
    base = path or GAME
    if not os.path.isdir(base):
        raise WriteError("%s is not a directory" % base)
    found = []
    for root, _dirs, names in os.walk(base):
        for name in names:
            if os.path.splitext(name)[1].lower() == ".hi":
                found.append(os.path.join(root, name))
    return sorted(found)


def _tmp_root() -> str:
    override = os.environ.get("GM_HIWRITE_TMPDIR")
    if override:
        os.makedirs(override, exist_ok=True)
        return override
    for base in (TMP_BASE, "/var/tmp", tempfile.gettempdir()):
        try:
            os.makedirs(base, exist_ok=True)
            probe = tempfile.mkdtemp(prefix="hiwrite-probe-", dir=base)
            os.rmdir(probe)
            return base
        except OSError:
            continue
    raise WriteError("no writable temp directory (set GM_HIWRITE_TMPDIR)")


def roundtrip(path: str = "", keep: bool = False) -> dict:
    """Read every ``*.hi``, write the model back, compare byte for byte."""
    files = hi_files(path)
    if not files:
        raise WriteError("no *.hi files under %s" % (path or GAME))
    if path and os.path.isfile(path):
        root_dir = os.path.dirname(os.path.abspath(path))
    else:
        root_dir = path or GAME
    base = _tmp_root()
    work = tempfile.mkdtemp(prefix="hiwrite-roundtrip-", dir=base)
    started = time.time()
    results = []
    totals = {"identical": 0, "differs": 0, "errors": 0, "bytes": 0,
              "records": 0, "oracle_records": 0, "oracle_best": 0,
              "toolkit_records": 0, "toolkit_bytes": 0}
    for src in files:
        rel = os.path.relpath(src, root_dir).replace(os.sep, "/")
        row = {"file": rel, "bytes": 0, "records": 0, "problems": []}
        raw = read_bytes(src)
        row["bytes"] = len(raw)
        totals["bytes"] += len(raw)
        try:
            records = parse(raw, rel)
        except WriteError as exc:
            totals["errors"] += 1
            row.update(status="error", reason=str(exc))
            results.append(row)
            continue
        row["records"] = len(records)
        totals["records"] += len(records)
        out = os.path.join(work, rel)
        write_bytes(out, raw)          # "write it back" to /tmp
        written = to_bytes(records)
        write_bytes(out, written)
        back = read_bytes(out)
        if written == raw and back == raw:
            totals["identical"] += 1
            row["status"] = "identical"
        else:
            totals["differs"] += 1
            stat = diff_stats(raw, written)
            row["status"] = "differs"
            row["reason"] = ("re-serialised %d B != %d B; %d byte(s) differ, "
                             "first at offset %s"
                             % (len(written), len(raw), stat["changed"],
                                stat["first_diff"]))
            row["problems"].append(row["reason"])
        row["sha256"] = sha256(raw)
        if hioracle is not None:
            want = oracle_view(records)
            if hioracle.parse_hi(raw) == want:
                totals["oracle_records"] += 1
                # Both models write zeros after the NUL, so `encode_hi` can only
                # reproduce a file whose tail is already clean; a dirty tail is
                # reported once, as `differs`, not twice.
                if to_bytes(records) == raw and hioracle.encode_hi(want) != raw:
                    row["problems"].append("campaign_flow.encode_hi differs")
            else:
                row["problems"].append("campaign_flow.parse_hi differs")
            ours = signed_best(records)
            theirs = {fold(k): v
                      for k, v in hioracle.best_by_name(want).items()}
            if ours == theirs:
                totals["oracle_best"] += 1
            else:
                row["problems"].append("minimum per name differs from the "
                                       "oracle")
        else:
            row["problems"].append("oracle unavailable: %s" % ORACLE_ERROR)
        if smallmod is not None:
            try:
                scores = smallmod.parse_hi(raw)
                names = [smallmod.record_name(r) for r in scores.records]
                values = [to_i32(r["score"]) for r in scores.records]
                if names == [r["name"] for r in records] and \
                        values == [to_i32(r["seconds"]) for r in records]:
                    totals["toolkit_records"] += 1
                else:
                    row["problems"].append("gm/small.py sees other records")
                if scores.to_bytes() == raw:
                    totals["toolkit_bytes"] += 1
                else:
                    row["problems"].append("gm/small.py to_bytes differs")
            except (ValueError, TypeError) as exc:
                row["problems"].append("gm/small.py: %s" % exc)
        else:
            row["problems"].append("gm/small.py unavailable: %s"
                                   % TOOLKIT_ERROR)
        results.append(row)
    if not keep:
        shutil.rmtree(work, ignore_errors=True)
        work_dir = work + " (removed)"
    else:
        work_dir = work
    rep = {
        "tool": "hiwrite", "mode": "roundtrip",
        "dir": os.path.abspath(path or GAME),
        "files": len(files), "bytes": totals["bytes"],
        "records": totals["records"],
        "identical": totals["identical"], "differs": totals["differs"],
        "errors": totals["errors"],
        "oracle": "ok" if hioracle is not None else "unavailable",
        "oracle_path": ORACLE_PATH,
        "oracle_error": ORACLE_ERROR,
        "oracle_records_match": totals["oracle_records"],
        "oracle_best_match": totals["oracle_best"],
        "toolkit": "ok" if smallmod is not None else "unavailable",
        "toolkit_error": TOOLKIT_ERROR,
        "toolkit_records_match": totals["toolkit_records"],
        "toolkit_bytes_match": totals["toolkit_bytes"],
        "seconds": round(time.time() - started, 3),
        "tmpdir": work_dir,
        "results": results,
    }
    rep["ok"] = (totals["identical"] == len(files) and not totals["differs"]
                 and not totals["errors"]
                 and totals["oracle_records"] == len(files)
                 and totals["oracle_best"] == len(files)
                 and totals["toolkit_records"] == len(files)
                 and totals["toolkit_bytes"] == len(files))
    return rep


def print_roundtrip(rep) -> None:
    print("roundtrip: %s" % rep["dir"])
    print("  %d files, %d records, %d bytes, %.3fs"
          % (rep["files"], rep["records"], rep["bytes"], rep["seconds"]))
    print("  identical=%d/%d  differs=%d  errors=%d"
          % (rep["identical"], rep["files"], rep["differs"], rep["errors"]))
    print("  oracle campaign_flow.py: records match=%d/%d, "
          "minimum per name match=%d/%d"
          % (rep["oracle_records_match"], rep["files"],
             rep["oracle_best_match"], rep["files"]))
    print("  toolkit gm/small.py: records match=%d/%d, to_bytes identical=%d/%d"
          % (rep["toolkit_records_match"], rep["files"],
             rep["toolkit_bytes_match"], rep["files"]))
    print("  tmp: %s" % rep["tmpdir"])
    for row in rep["results"]:
        if row["status"] != "identical":
            print("  %-28s %-9s %s"
                  % (row["file"], row["status"], row.get("reason", "")))
        for problem in row["problems"]:
            if problem != row.get("reason"):
                print("  problem: %s: %s" % (row["file"], problem))
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# self-test (no game files)


def _selftest_workdir() -> str:
    base = "/var/tmp"
    if not os.path.isdir(base) or not os.access(base, os.W_OK):
        base = TMP_BASE
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = None
    return tempfile.mkdtemp(prefix="hiwrite-selftest-", dir=base)


def shipped_shape() -> bytes:
    """A synthetic table with the shipped times/names (no game file needed)."""
    rows = [(146, "TESTING RANGE"), (1570, "KELLER CORE"),
            (18, "TESTING RANGE"), (109, "POWER STATION"),
            (18, "NATAKA TOWER"), (7, "NATAKA TOWER"), (11, "OIL RIG"),
            (40, "OIL REFINERY"), (6, "CADTLELTL")]
    return to_bytes([{"seconds": s, "name": n} for s, n in rows])


def _selftest(as_json: bool = False) -> bool:
    results = []
    work = _selftest_workdir()
    good = shipped_shape()

    def case(name, fn):
        try:
            detail = fn()
            results.append((name, True, detail or ""))
        except Exception as exc:                        # noqa: BLE001
            results.append((name, False, "%s: %s"
                            % (type(exc).__name__, exc)))

    def expect_error(fn, needle):
        try:
            fn()
        except WriteError as exc:
            if needle not in str(exc):
                raise AssertionError("message %r lacks %r" % (str(exc), needle))
            return "refused: %s" % str(exc)[:70]
        raise AssertionError("expected WriteError containing %r" % needle)

    def path(name, data=None):
        p = os.path.join(work, name)
        if data is not None:
            os.makedirs(os.path.dirname(p) or work, exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        return p

    def t_shape():
        assert len(good) == SHIPPED_BYTES, len(good)
        assert len(parse(good)) == SHIPPED_RECORDS
        return "%d bytes, %d records" % (len(good), len(parse(good)))

    def t_roundtrip():
        assert to_bytes(parse(good)) == good
        return "576/576 bytes identical"

    def t_little_endian():
        rec = parse(good)[0]
        assert rec["seconds"] == 146
        assert rec["raw_seconds"] == b"\x92\x00\x00\x00", rec["raw_seconds"]
        assert parse(b"\x22\x06\x00\x00" + b"X" + b"\x00" * 59)[0]["seconds"] \
            == 1570
        return "146 -> 92 00 00 00"

    def t_name_field():
        rec = parse(good)[0]
        assert rec["name"] == "TESTING RANGE" and len(rec["name_bytes"]) == 13
        assert rec["padding"] == 47 and rec["terminated"]
        assert good[4 + 13] == 0
        return "13-byte name + NUL + 46 zeros"

    def t_slots_offsets():
        rows = parse(good)
        assert [r["offset"] for r in rows] == list(range(0, 576, 64))
        assert [r["slot"] for r in rows] == list(range(9))
        return "slots 0..8 at 0,64,...,512"

    def t_tail_garbage():
        raw = bytearray(good)
        raw[4 + 13 + 1] = 0xFF
        rec = parse(bytes(raw))[0]
        assert rec["tail_nonzero"] == 1
        assert to_bytes(parse(bytes(raw))) != bytes(raw)
        return "non-zero tail is reported, not normalised"

    def t_no_terminator():
        raw = bytearray(good)
        raw[4:64] = b"A" * 60
        rec = parse(bytes(raw))[0]
        assert not rec["terminated"] and len(rec["name"]) == 60
        assert to_bytes(parse(bytes(raw))) == bytes(raw)
        problems = doc_problems(analyse(bytes(raw)), "x")["problems"]
        assert any("no NUL" in p for p in problems), problems
        return "60-byte name without NUL is a problem"

    def t_load_size():
        p = path("ok.hi", good)
        assert load(p)["bytes"] == 576
        return "576 accepted"

    def t_load_short():
        p = path("short.hi", good[:575])
        return expect_error(lambda: load(p), "is 575 bytes")

    def t_load_long():
        p = path("long.hi", good + b"\x00" * 64)
        return expect_error(lambda: load(p), "is 640 bytes")

    def t_partial_record():
        return expect_error(lambda: parse(b"\x00" * 63, "x"),
                            "not a multiple of 64")

    def t_check_ok():
        rep = check_file(path("check-ok.hi", good))
        assert rep["ok"] and not rep["problems"], rep["problems"]
        st = rep["stats"]
        assert (st["records"], st["filled"], st["empty"]) == (9, 9, 0), st
        assert (st["time_min"], st["time_max"]) == (6, 1570), st
        assert st["times"] == [146, 1570, 18, 109, 18, 7, 11, 40, 6], st
        assert st["distinct_names"] == 7 and st["duplicate_rows"] == 2, st
        return "9/9 filled, times 6..1570"

    def t_check_negative_time():
        raw = bytearray(good)
        raw[0:4] = b"\xff\xff\xff\xff"
        rep = check_file(path("neg.hi", bytes(raw)))
        assert rep["ok"], rep["problems"]
        assert any("above 0x7FFFFFFF" in w for w in rep["warnings"]), \
            rep["warnings"]
        return "u32 0xFFFFFFFF is a note (i32 readers refuse it)"

    def t_check_non_ascii():
        raw = bytearray(good)
        raw[4] = ord("A")
        raw[5] = 0xE9
        rep = check_file(path("nonascii.hi", bytes(raw)))
        assert not rep["ok"], rep
        assert any("not printable ASCII" in p for p in rep["problems"]), rep
        return "non-ASCII name is a structural problem, exit 1"

    def t_against_identical():
        p = path("against-a.hi", good)
        rep = check_file(p, p)
        assert rep["identical"] and rep["changed_bytes"] == 0
        return "IDENTICAL, 0 bytes"

    def t_against_diff():
        p = path("against-b.hi", good)
        rep = check_file(path("against-a2.hi", good), p)
        assert rep["identical"] and rep["first_changed_slot"] is None
        return "same bytes, no slot"

    def t_list():
        rep = list_records(path("list.hi", good))
        assert rep["records"] == 9
        rows = rep["rows"]
        assert rows[0]["seconds"] == 146 and rows[0]["time"] == "2 : 26"
        assert rows[1]["time"] == "26 : 10"
        assert rows[2]["duplicate"] and not rows[0]["duplicate"]
        assert rows[8]["name"] == "CADTLELTL" and rows[8]["offset"] == 512
        return "9 rows, time text 2 : 26 / 26 : 10"

    def t_audit():
        rep = audit_file(path("audit.hi", good))
        assert rep["ok"] and rep["times"][1] == 1570
        best = {entry["name"]: (entry["seconds"], entry["entries"])
                for entry in rep["best"]}
        assert best["KELLER CORE"] == (1570, 1), best
        assert best["TESTING RANGE"] == (18, 2), best
        assert len(rep["best"]) == 7
        assert [e["seconds"] for e in rep["best"]] == sorted(
            e["seconds"] for e in rep["best"])
        json.dumps(rep)
        return "7 names, minima 18/6/7/11/40/109/1570"

    def t_audit_refuses_size():
        return expect_error(lambda: audit_file(path("audit-575.hi", good[:575])),
                            "is 575 bytes")

    def t_set_time_one_byte():
        src = path("set-t.hi", good)
        out = os.path.join(work, "out", "set-t.hi")
        rep = set_record(src, 2, out, seconds=5)
        assert rep["changed_bytes"] == 1, rep
        assert rep["changed_in_record"] == 1
        assert rep["first_diff"] == 2 * 64 + 0, rep["first_diff"]
        after = read_bytes(out)
        assert after[2 * 64:2 * 64 + 4] == b"\x05\x00\x00\x00"
        assert after[:2 * 64] == good[:2 * 64]
        assert after[2 * 64 + 4:] == good[2 * 64 + 4:]
        return "slot 2: 18 -> 5, exactly 1 byte"

    def t_set_time_four_bytes():
        src = path("set-t4.hi", good)
        out = os.path.join(work, "out", "set-t4.hi")
        rep = set_record(src, 0, out, seconds=0x01020304)
        assert rep["changed_bytes"] == 4, rep["changed_bytes"]
        assert read_bytes(out)[:4] == b"\x04\x03\x02\x01"
        return "slot 0: 4 bytes change"

    def t_set_time_same():
        src = path("set-same.hi", good)
        out = os.path.join(work, "out", "set-same.hi")
        rep = set_record(src, 4, out, seconds=18)
        assert rep["changed_bytes"] == 0 and read_bytes(out) == good
        return "same value: 0 bytes"

    def t_set_time_range():
        src = path("set-r.hi", good)
        out = os.path.join(work, "out", "set-r.hi")
        expect_error(lambda: set_record(src, 0, out, seconds=-1),
                     "outside 0..0xFFFFFFFF")
        expect_error(lambda: set_record(src, 0, out, seconds=0x100000000),
                     "outside 0..0xFFFFFFFF")
        expect_error(lambda: set_record(src, 0, out, seconds="12x"),
                     "not an integer")
        rep = set_record(src, 0, out, seconds="0xFF")
        assert rep["after"]["seconds"] == 255
        return "0..u32 accepted, -1/2**32 refused"

    def t_set_slot_range():
        src = path("set-slot.hi", good)
        out = os.path.join(work, "out", "set-slot.hi")
        expect_error(lambda: set_record(src, -1, out, seconds=1),
                     "out of range")
        expect_error(lambda: set_record(src, 9, out, seconds=1),
                     "out of range")
        return "slots -1 and 9 refused"

    def t_set_name():
        src = path("set-n.hi", good)
        out = os.path.join(work, "out", "set-n.hi")
        rep = set_record(src, 7, out, name="KELLER CORE")
        after = read_bytes(out)
        assert after[7 * 64 + 4:7 * 64 + 64] == \
            b"KELLER CORE" + b"\x00" * 49
        assert rep["changed_bytes"] >= 1 and rep["bytes_out"] == 576
        assert after[:7 * 64] == good[:7 * 64]
        assert after[7 * 64 + 60:] == good[7 * 64 + 60:]
        return "slot 7 renamed, 60-byte field zero-padded"

    def t_set_name_ascii():
        src = path("set-na.hi", good)
        out = os.path.join(work, "out", "set-na.hi")
        return expect_error(lambda: set_record(src, 0, out, name="CAF\u00e9"),
                            "not ASCII")

    def t_set_name_empty():
        src = path("set-ne.hi", good)
        out = os.path.join(work, "out", "set-ne.hi")
        return expect_error(lambda: set_record(src, 0, out, name=""), "empty")

    def t_set_name_control():
        src = path("set-nc.hi", good)
        out = os.path.join(work, "out", "set-nc.hi")
        return expect_error(lambda: set_record(src, 0, out, name="A\tB"),
                            "control byte")

    def t_set_name_length():
        src = path("set-nl.hi", good)
        out = os.path.join(work, "out", "set-nl.hi")
        rep = set_record(src, 8, out, name="Z" * 59)
        assert read_bytes(out)[8 * 64 + 4 + 59] == 0
        expect_error(lambda: set_record(src, 8, out, name="Z" * 60),
                     "at most 59")
        return "59 bytes fit, 60 refused"

    def t_set_empty_slot():
        raw = bytearray(good)
        raw[6 * 64 + 4:7 * 64] = b"\x00" * 60
        src = path("set-empty.hi", bytes(raw))
        out = os.path.join(work, "out", "set-empty.hi")
        assert any("empty slot" in w
                   for w in check_file(src)["warnings"])
        rep = set_record(src, 6, out, name="OIL RIG")
        assert rep["check_ok"] and read_bytes(out)[6 * 64 + 4:6 * 64 + 8] == \
            b"OIL "
        return "an empty slot gains a name, size unchanged"

    def t_guard():
        src = path("guard.hi", good)
        expect_error(lambda: set_record(src, 0, os.path.join(GAME, "x.hi"),
                                        seconds=1), "refusing to write")
        expect_error(lambda: set_record(src, 0,
                                        os.path.join(MIRROR_GAME, "x.hi"),
                                        seconds=1), "refusing to write")
        expect_error(lambda: set_record(src, 0, os.path.join(ROOT, "x.hi"),
                                        seconds=1), "refusing to write")
        expect_error(lambda: set_record(src, 0,
                                        os.path.join(MAIN_REPO, "x.hi"),
                                        seconds=1), "refusing to write")
        expect_error(lambda: set_record(src, 0, src, seconds=1),
                     "refusing to overwrite")
        return "game/mirror/worktree/main repo/input refused"

    def t_oracle():
        assert hioracle is not None, ORACLE_ERROR
        want = oracle_view(parse(good))
        assert hioracle.parse_hi(good) == want
        assert hioracle.encode_hi(want) == good
        ours = signed_best(parse(good))
        theirs = {fold(k): v for k, v in hioracle.best_by_name(want).items()}
        assert ours == theirs, (ours, theirs)
        big = parse(b"\xff\xff\xff\xff" + b"X" + b"\x00" * 59)
        assert oracle_view(big)[0]["seconds"] == -1
        assert hioracle.encode_hi([{"seconds": -1, "name": "X"}]) == \
            b"\xff\xff\xff\xffX" + b"\x00" * 59
        return "parse_hi/encode_hi/best_by_name agree; i32 view of 0xFFFFFFFF"

    def t_toolkit():
        assert smallmod is not None, TOOLKIT_ERROR
        scores = smallmod.parse_hi(good)
        assert len(scores.records) == 9
        assert [smallmod.record_name(r) for r in scores.records] == \
            [r["name"] for r in parse(good)]
        assert [r["score"] for r in scores.records] == \
            [r["seconds"] for r in parse(good)]
        assert scores.to_bytes() == good
        return "gm/small.py parse_hi/to_bytes agree, 576/576"

    def t_best_fold():
        recs = parse(to_bytes([{"seconds": 5, "name": "OIL RIG"},
                               {"seconds": 3, "name": "oil rig"}]))
        best = best_by_name(recs)
        assert len(best) == 1 and list(best.values())[0]["seconds"] == 3
        assert list(best.values())[0]["entries"] == 2
        return "0x416850 fold: one name, minimum 3, 2 entries"

    def t_roundtrip_synth():
        d = os.path.join(work, "rt")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "gunmetal.hi"), "wb") as f:
            f.write(good)
        with open(os.path.join(d, "other.HI"), "wb") as f:
            f.write(to_bytes([{"seconds": 7, "name": "SINGLE"}]))
        rep = roundtrip(d)
        assert rep["files"] == 2 and rep["identical"] == 2, rep
        assert rep["records"] == 10 and rep["bytes"] == 576 + 64
        assert rep["oracle_records_match"] == 2
        return "2 files (1 record + 9), identical=2/2"

    def t_roundtrip_one_file():
        clean = path("one.hi", good)
        rep = roundtrip(clean)
        assert rep["files"] == 1 and rep["identical"] == 1 and rep["ok"], rep
        dirty = bytearray(good)
        dirty[4 + 13 + 1] = 0xAA
        bad = path("dirty.hi", bytes(dirty))
        rep = roundtrip(bad)
        assert rep["files"] == 1 and rep["differs"] == 1, rep
        assert not rep["ok"] and rep["results"][0]["problems"], rep
        assert rep["toolkit_bytes_match"] == 1, rep
        return "single file: 1/1 clean, 1/1 dirty reported, toolkit keeps raw"

    def t_roundtrip_bad_tail():
        d = os.path.join(work, "rt-bad")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "bad.hi"), "wb") as f:
            f.write(good + b"\x00" * 3)
        rep = roundtrip(d, keep=True)
        assert rep["files"] == 1 and rep["errors"] == 1 and not rep["ok"]
        assert "not a multiple" in rep["results"][0]["reason"]
        shutil.rmtree(rep["tmpdir"], ignore_errors=True)
        return "partial tail is an error, not a rounded file"

    def t_esc():
        assert esc("A\x00\xff") == "A\\x00\\xff"
        assert esc("A\tB") == "A\\tB"
        return "byte-faithful display"

    def t_helpers():
        assert diff_stats(b"AB", b"AC") == {"changed": 1, "first_diff": 1,
                                            "delta": 0}
        assert sha256(b"") == ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b93"
                               "4ca495991b7852b855")
        assert fnv1a64(good) == 0x80BF2F264BEBE680, hex(fnv1a64(good))
        return "diff/sha/FNV-1a match the documented fingerprint"

    def t_time_text():
        assert time_text(6) == "0 : 06" and time_text(109) == "1 : 49"
        assert time_text(1570) == "26 : 10" and time_text(0) == "UNKNOWN"
        return "0x483f00 format"

    for name, fn in [
            ("shape: 9 x 0x40", t_shape),
            ("round-trip is identity", t_roundtrip),
            ("time is u32 little-endian", t_little_endian),
            ("name field: NUL + padding", t_name_field),
            ("slots and offsets", t_slots_offsets),
            ("garbage after NUL reported", t_tail_garbage),
            ("60-byte name without NUL", t_no_terminator),
            ("load: 576 bytes", t_load_size),
            ("load: 575 refused", t_load_short),
            ("load: 640 refused", t_load_long),
            ("parse: partial record refused", t_partial_record),
            ("check: shipped shape ok", t_check_ok),
            ("check: u32 above i32 noted", t_check_negative_time),
            ("check: non-ASCII is a problem", t_check_non_ascii),
            ("check: --against identical", t_against_identical),
            ("check: --against same slots", t_against_diff),
            ("list: slots/times/names", t_list),
            ("audit: numbers and minima", t_audit),
            ("audit: 575 refused", t_audit_refuses_size),
            ("set-time: 1 byte", t_set_time_one_byte),
            ("set-time: 4 bytes", t_set_time_four_bytes),
            ("set-time: same value 0 bytes", t_set_time_same),
            ("set-time: u32 range", t_set_time_range),
            ("set-time: slot range", t_set_slot_range),
            ("set-name: field rewritten", t_set_name),
            ("set-name: non-ASCII refused", t_set_name_ascii),
            ("set-name: empty refused", t_set_name_empty),
            ("set-name: control byte", t_set_name_control),
            ("set-name: 59 ok, 60 refused", t_set_name_length),
            ("set-name: empty slot filled", t_set_empty_slot),
            ("guard: game/mirror/repo/input", t_guard),
            ("oracle campaign_flow agrees", t_oracle),
            ("toolkit gm/small.py agrees", t_toolkit),
            ("best: ASCII fold", t_best_fold),
            ("roundtrip: synthetic dir", t_roundtrip_synth),
            ("roundtrip: one file", t_roundtrip_one_file),
            ("roundtrip: partial tail", t_roundtrip_bad_tail),
            ("helpers: escaping", t_esc),
            ("helpers: diff/sha/FNV", t_helpers),
            ("time text 0x483f00", t_time_text),
    ]:
        case(name, fn)

    shutil.rmtree(work, ignore_errors=True)
    passed = sum(1 for _n, ok, _d in results if ok)
    if as_json:
        print(json.dumps({
            "tool": "hiwrite", "mode": "self-test",
            "passed": passed, "total": len(results),
            "ok": passed == len(results),
            "oracle": "ok" if hioracle is not None else "unavailable",
            "checks": [{"name": name, "ok": ok, "detail": detail}
                       for name, ok, detail in results],
        }, indent=1))
    else:
        for name, ok, detail in results:
            print("  %-34s %s  %s" % (name, "ok  " if ok else "FAIL", detail))
        print("self-test: %d/%d" % (passed, len(results)))
    return passed == len(results)


# ---------------------------------------------------------------------------
# CLI


def _cmd_roundtrip(args) -> int:
    rep = roundtrip(args.roundtrip or "", args.keep)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_roundtrip(rep)
        if args.verbose:
            for row in rep["results"]:
                print("  %-28s %-9s %d records, %d bytes, sha256=%s"
                      % (row["file"], row["status"], row["records"],
                         row["bytes"], row.get("sha256", "")[:16]))
    return 0 if rep["ok"] else 1


def _cmd_audit(args) -> int:
    rep = audit_file(args.audit or "")
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_audit(rep)
    return 0 if rep["ok"] else 1


def _cmd_list(args) -> int:
    rep = list_records(args.list or default_file())
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_list(rep)
    return 0


def _cmd_check(args) -> int:
    rep = check_file(args.check, args.against)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_check(rep)
    return 0 if rep["ok"] else 1


def _cmd_set(args, which: str) -> int:
    if not args.out:
        raise WriteError("--%s needs --out OUT" % which)
    src = args.infile or default_file()
    if which == "set-time":
        if args.seconds is None:
            raise WriteError("--set-time needs --seconds N")
        rep = set_record(src, args.set_time, args.out, seconds=args.seconds)
    else:
        if args.name is None:
            raise WriteError("--set-name needs --name TEXT")
        rep = set_record(src, args.set_name, args.out, name=args.name)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_set(rep)
    return 0 if rep["check_ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(
        description="Writer/editor for the Gunmetal GUNMETAL.HI best-time "
                    "table (M11.5): 9 records of u32 seconds + char name[60].")
    ap.add_argument("--roundtrip", nargs="?", const="", metavar="PATH",
                    help="read every *.hi, write the model back to a temp "
                         "dir and compare byte for byte (default game dir)")
    ap.add_argument("--audit", nargs="?", const="", metavar="FILE",
                    help="numbers of one table (default <game>/GUNMETAL.HI)")
    ap.add_argument("--list", nargs="?", const="", metavar="FILE",
                    help="slot / seconds / time text / name")
    ap.add_argument("--check", metavar="FILE",
                    help="structural check of one 576-byte table")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: also compare byte for byte with FILE")
    ap.add_argument("--set-time", type=int, metavar="SLOT", dest="set_time",
                    help="0-based slot whose 4 time bytes are replaced")
    ap.add_argument("--set-name", type=int, metavar="SLOT", dest="set_name",
                    help="0-based slot whose 60 name bytes are replaced")
    ap.add_argument("--seconds", metavar="N",
                    help="new time (decimal or 0x...), 0..0xFFFFFFFF")
    ap.add_argument("--name", metavar="TEXT", help="new level name (ASCII)")
    ap.add_argument("--in", "--input", dest="infile", metavar="FILE",
                    help="input table (default <game>/GUNMETAL.HI)")
    ap.add_argument("--out", metavar="PATH", help="output file")
    ap.add_argument("--keep", action="store_true",
                    help="--roundtrip: keep the temp copies")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic tests (no game files)")
    ap.add_argument("--game", default=None,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.game:
        global GAME
        GAME = args.game

    if args.self_test:
        return 0 if _selftest(args.json) else 1
    chosen = [x for x in (args.roundtrip, args.audit, args.list, args.check,
                          args.set_time, args.set_name) if x is not None]
    if len(chosen) > 1:
        ap.error("choose one of --roundtrip/--audit/--list/--check/"
                 "--set-time/--set-name")
    if args.roundtrip is not None:
        return _cmd_roundtrip(args)
    if args.audit is not None:
        return _cmd_audit(args)
    if args.list is not None:
        return _cmd_list(args)
    if args.check:
        return _cmd_check(args)
    if args.set_time is not None:
        return _cmd_set(args, "set-time")
    if args.set_name is not None:
        return _cmd_set(args, "set-name")
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except WriteError as exc:
        print("hiwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, OSError) as exc:
        print("hiwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                           # noqa: BLE001
        print("hiwrite: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
