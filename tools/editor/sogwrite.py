#!/usr/bin/env python3
"""Writer/editor for the Gunmetal ``.sog`` song playlists (``M11.5``, slice 5).

A ``.sog`` file is **plain text**: one sample name per line, CRLF endings, no
header, no checksum, no offsets (``docs/FORMATS.md`` §9, ``gm/sog.py``).  That
makes a *byte-exact* writer a line model, not a binary splice::

    raw = CRLF.join(line.encode("latin1") for line in lines)

Splitting on ``\\r\\n`` and joining back is the identity for every shipped file
(651/651: 598 ``.sog`` + 53 ``.SOG``), including

* empty records (``Dm1A.sog`` ends with two CRLF: the last record is empty);
* lines with leading/trailing spaces (``L01C.sog``: ``L01C01 \\r\\n``);
* a last line **without** a final CRLF (7 files, e.g. ``L01C.sog``);
* duplicate names (998 extra occurrences in 131 playlists -- that is music,
  not garbage: ``docs/PLAYLIST.md`` §1).

The reader is :mod:`gm.sog` (the oracle, used here unchanged for cross-checks,
never written).  This tool only *writes*, and only ever replaces the name of
**one existing record**: it never creates or deletes a line, never converts
line endings, never touches the surrounding spaces of the edited record and
never expands ``CR``/``LF``.

Usage::

    python3 tools/editor/sogwrite.py --roundtrip            # game/MUSIC, recursive
    python3 tools/editor/sogwrite.py --roundtrip DIR
    python3 tools/editor/sogwrite.py --audit                # corpus numbers
    python3 tools/editor/sogwrite.py --list MUSIC/Dm1A.sog
    python3 tools/editor/sogwrite.py --check MUSIC/Dm1A.sog
    python3 tools/editor/sogwrite.py --check OUT.sog --against MUSIC/Dm1A.sog
    python3 tools/editor/sogwrite.py --set MUSIC/Dm1A.sog --line 3 \
            --name Dm1A09 --out /tmp/opencode/Dm1A.sog
    python3 tools/editor/sogwrite.py --self-test

Exit codes: ``0`` success, ``1`` verification failed (roundtrip/check/self-test
negative), ``2`` bad input or usage (message on stderr), ``3`` internal error.

Refusals (code 2): not ``.sog``/``.SOG``, an empty file, mixed/lone ``CR``/``LF``
line endings, ``--out`` inside the game installation / its regression mirror /
this repository, ``--out`` equal to the input, or an edit that would change the
line layout.  Nothing is ever written into the game installation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sog as sogmod                    # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")
#: the game installation in use (``--game`` overrides it)
GAME = DEFAULT_GAME

SOG = ".sog"
CRLF = b"\r\n"
#: ``char name[8]`` of a BIGFILE entry -- a longer playlist name can never
#: resolve (``docs/PLAYLIST.md`` §2, ``docs/FORMATS.md`` §1)
ARCHIVE_NAME_BYTES = 8
#: trust boundary of the C++ reader (``PlaylistEntry``, ``res/text.h``)
MAX_NAME_BYTES = 255
#: characters ``str.strip()`` removes; the oracle trims with plain ``strip()``
SPACES = " \t\n\r\v\f"


class WriteError(Exception):
    """Bad input or an impossible edit; the message is meant for the user."""


# ---------------------------------------------------------------------------
# small helpers


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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


def guard_roots() -> tuple:
    """The reference installation, its mirror and this repository."""
    return (GAME, DEFAULT_GAME, MIRROR_GAME, ROOT)


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


def kind_of(path: str) -> str:
    ext = os.path.splitext(path)[1]
    if ext.lower() != SOG:
        raise WriteError("%s: not a .sog file (extension %r)"
                         % (path, ext or "<none>"))
    return ext


def sog_files(directory: str) -> list:
    """Every ``.sog``/``.SOG`` under *directory* (recursive, case-insensitive)."""
    out = []
    for base, _dirs, names in os.walk(directory):
        for name in names:
            if os.path.splitext(name)[1].lower() == SOG:
                out.append(os.path.join(base, name))
    return sorted(out)


def esc(text: str) -> str:
    """A printable, byte-faithful rendering of a latin1 line."""
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
    return {"changed": changed, "first_diff": first, "delta": len(b) - len(a)}


# ---------------------------------------------------------------------------
# document model (a line list; ``gm/sog.py`` is the reader)


def to_bytes(lines) -> bytes:
    """Serialise records with CRLF; no trailing EOL is added or removed."""
    return CRLF.join(line.encode("latin1") for line in lines)


def _lone_eol_offset(raw: bytes):
    """Offset of the first ``CR``/``LF`` that is not part of a CRLF pair."""
    i = 0
    size = len(raw)
    while i < size:
        byte = raw[i]
        if byte == 0x0D:
            if i + 1 < size and raw[i + 1] == 0x0A:
                i += 2
                continue
            return i
        if byte == 0x0A:
            return i
        i += 1
    return None


def analyse(raw: bytes, where: str = "") -> dict:
    """Split a playlist into exact records; refuse what the model cannot hold."""
    name = where or "<bytes>"
    if not raw:
        raise WriteError("empty playlist: %s" % name)
    bad = _lone_eol_offset(raw)
    if bad is not None:
        raise WriteError(
            "%s: byte 0x%02x at offset %d is a lone CR/LF; only CRLF records "
            "are modelled (all 651 shipped files are pure CRLF)"
            % (name, raw[bad], bad))
    lines = raw.decode("latin1").split("\r\n")
    names = [ln.strip() for ln in lines if ln.strip()]
    return {
        "raw": raw,
        "bytes": len(raw),
        "lines": lines,
        "names": names,
        "final_eol": raw.endswith(CRLF),
        "records": len(lines),
    }


def load(path: str) -> dict:
    kind_of(path)
    doc = analyse(read_bytes(path), path)
    doc["path"] = path
    return doc


def record_rows(lines) -> list:
    """One row per record: index (1-based), byte offset, length, name, flags."""
    rows = []
    offset = 0
    folded = Counter(ln.strip().lower() for ln in lines if ln.strip())
    seen = Counter()
    for i, line in enumerate(lines):
        raw = line.encode("latin1")
        core = line.strip()
        flags = []
        if not core:
            flags.append("ws-only" if line else "blank")
        elif line != core:
            flags.append("outer-ws")
        if len(raw) > MAX_NAME_BYTES:
            flags.append("line>255B")
        if core:
            name_bytes = len(core.encode("latin1"))
            if name_bytes > ARCHIVE_NAME_BYTES:
                flags.append("name>8B")
            key = core.lower()
            seen[key] += 1
            if folded[key] > 1:
                flags.append("dup%d" % seen[key])
            if any(ch in SPACES for ch in core) or any(
                    ord(ch) < 0x20 or ord(ch) == 0x7F for ch in core):
                flags.append("odd-name")
        rows.append({"index": i + 1, "offset": offset, "length": len(raw),
                     "name": core, "text": line, "flags": flags})
        offset += len(raw) + len(CRLF)
    return rows


def doc_stats(lines) -> dict:
    """Numbers of one playlist (they are *data*: blanks, dupes, spaces)."""
    rows = record_rows(lines)
    folded = Counter(r["name"].lower() for r in rows if r["name"])
    name_lens = [len(r["name"].encode("latin1")) for r in rows if r["name"]]
    line_lens = [r["length"] for r in rows]
    exact = Counter(r["name"] for r in rows if r["name"])
    return {
        "records": len(rows),
        "blank": sum(1 for r in rows if not r["name"]),
        "blank_ws_only": sum(1 for r in rows if not r["name"] and r["text"]),
        "outer_ws": sum(1 for r in rows if r["name"] and r["text"] != r["name"]),
        "names": len(name_lens),
        "dups": sum(v - 1 for v in folded.values()),
        "dups_exact": sum(v - 1 for v in exact.values()),
        "dup_names": sum(1 for v in folded.values() if v > 1),
        "distinct": len(folded),
        "name_len_min": min(name_lens) if name_lens else 0,
        "name_len_max": max(name_lens) if name_lens else 0,
        "line_len_min": min(line_lens) if line_lens else 0,
        "line_len_max": max(line_lens) if line_lens else 0,
        "bytes": sum(r["length"] for r in rows) + max(0, len(rows) - 1) * 2,
    }


# ---------------------------------------------------------------------------
# --check


def doc_problems(doc: dict, where: str) -> dict:
    """Structural verdict: problems (refuse) vs notes/warnings (data)."""
    problems, warnings = [], []
    rows = record_rows(doc["lines"])
    names = doc["names"]
    if not names:
        problems.append("no sample names (whitespace-only playlist)")
    nul = len(doc["raw"]) - len(doc["raw"].replace(b"\x00", b""))
    if nul:
        problems.append("%d NUL byte(s): a name would be cut at the NUL" % nul)
    high = sorted({b for b in doc["raw"] if b > 0x7F})
    if high:
        warnings.append(
            "non-ASCII byte(s) %s: legal for the reader (latin1, compared "
            "bytewise) but absent from the shipped corpus"
            % ", ".join("0x%02x" % b for b in high[:8]))
    long_names = [r for r in rows if r["name"] and
                  len(r["name"].encode("latin1")) > ARCHIVE_NAME_BYTES]
    if long_names:
        warnings.append(
            "%d name(s) longer than %d bytes (first: line %d %r): the BIGFILE "
            "field is name[8], so such a name never resolves "
            "(docs/PLAYLIST.md §2)"
            % (len(long_names), ARCHIVE_NAME_BYTES, long_names[0]["index"],
               long_names[0]["name"]))
    odd = [r for r in rows if "odd-name" in r["flags"]]
    if odd:
        warnings.append("%d name(s) with inner whitespace/control characters "
                        "(first: line %d %r)"
                        % (len(odd), odd[0]["index"], odd[0]["name"]))
    stats = doc_stats(doc["lines"])
    if stats["dups"]:
        warnings.append("%d duplicate occurrence(s) in %d distinct name(s) -- "
                        "kept as-is, they are part of the song "
                        "(docs/PLAYLIST.md §1)"
                        % (stats["dups"], stats["dup_names"]))
    if stats["blank"]:
        warnings.append("%d blank record(s)" % stats["blank"])
    if stats["outer_ws"]:
        warnings.append("%d name(s) surrounded by spaces (trimmed by the "
                        "reader, preserved here)" % stats["outer_ws"])
    if not doc["final_eol"]:
        warnings.append("last record has no CRLF (7 of 651 shipped files)")
    return {"problems": problems, "warnings": warnings, "stats": stats,
            "records": rows}


def check_file(path: str, against: str | None = None) -> dict:
    doc = load(path)
    verdict = doc_problems(doc, path)
    rep = {
        "tool": "sogwrite", "mode": "check", "file": os.path.abspath(path),
        "bytes": doc["bytes"], "records": len(doc["lines"]),
        "final_eol": doc["final_eol"], "sha256": sha256(doc["raw"]),
        "stats": verdict["stats"],
        "problems": verdict["problems"], "warnings": verdict["warnings"],
    }
    rep["structure_ok"] = not verdict["problems"]
    rep["ok"] = rep["structure_ok"]
    if against:
        other = load(against)
        cmp = diff_stats(doc["raw"], other["raw"])
        line_diff = None
        if len(doc["lines"]) == len(other["lines"]):
            for i, (a, b) in enumerate(zip(other["lines"], doc["lines"])):
                if a != b:
                    line_diff = {"index": i + 1, "before": a, "after": b}
                    break
        rep["against"] = os.path.abspath(against)
        rep["against_bytes"] = other["bytes"]
        rep["against_sha256"] = sha256(other["raw"])
        rep["identical"] = doc["raw"] == other["raw"]
        rep["changed_bytes"] = cmp["changed"]
        rep["first_diff"] = cmp["first_diff"]
        rep["delta"] = cmp["delta"]
        rep["records_before"] = len(other["lines"])
        rep["first_changed_record"] = line_diff
    return rep


def print_check(rep) -> None:
    print("check: %s" % rep["file"])
    st = rep["stats"]
    print("  %d bytes, %d records, final CRLF=%s, sha256=%s"
          % (rep["bytes"], rep["records"],
             "yes" if rep["final_eol"] else "no", rep["sha256"][:16]))
    print("  names=%d blank=%d outer-ws=%d dups=%d distinct=%d "
          "name-len=%d..%d line-len=%d..%d"
          % (st["names"], st["blank"], st["outer_ws"], st["dups"],
             st["distinct"], st["name_len_min"], st["name_len_max"],
             st["line_len_min"], st["line_len_max"]))
    if rep.get("against"):
        print("  against %s (%d bytes): %s"
              % (rep["against"], rep["against_bytes"],
                 "IDENTICAL" if rep["identical"] else "differs"))
        if not rep["identical"]:
            print("    changed %d byte(s) (positional), first diff at %s, "
                  "delta %+d, records %d -> %d"
                  % (rep["changed_bytes"],
                     "offset %s" % rep["first_diff"]
                     if rep["first_diff"] is not None else "n/a",
                     rep["delta"], rep["records_before"], rep["records"]))
            ld = rep["first_changed_record"]
            if ld:
                print("    first changed record %d: %r -> %r"
                      % (ld["index"], ld["before"], ld["after"]))
    for warning in rep["warnings"]:
        print("  note: %s" % warning)
    for problem in rep["problems"]:
        print("  problem: %s" % problem)
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --list


def list_records(path: str) -> dict:
    doc = load(path)
    rows = record_rows(doc["lines"])
    return {"tool": "sogwrite", "mode": "list", "file": os.path.abspath(path),
            "bytes": doc["bytes"], "records": len(rows),
            "final_eol": doc["final_eol"], "rows": rows}


def print_list(rep) -> None:
    print("list: %s (%d records, %d bytes, final CRLF=%s)"
          % (rep["file"], rep["records"], rep["bytes"],
             "yes" if rep["final_eol"] else "no"))
    print("  %4s %6s %4s  %-10s %s" % ("#", "off", "len", "name", "flags"))
    for row in rep["rows"]:
        name = esc(row["name"]) if row["name"] else "<blank>"
        print("  %4d %6d %4d  %-10s %s"
              % (row["index"], row["offset"], row["length"], name,
                 ",".join(row["flags"])))


# ---------------------------------------------------------------------------
# --audit


def audit_corpus(directory: str = "") -> dict:
    if not directory:
        directory = os.path.join(GAME, "MUSIC")
    if not os.path.isdir(directory):
        raise WriteError("%s is not a directory" % directory)
    files = sog_files(directory)
    if not files:
        raise WriteError("no .sog files under %s" % directory)
    started = time.time()
    totals = Counter()
    name_lens = []
    line_lens = []
    folded = Counter()
    files_no_eol = []
    problems = []
    for path in files:
        raw = read_bytes(path)
        rel = os.path.relpath(path, directory).replace(os.sep, "/")
        try:
            doc = analyse(raw, rel)
        except WriteError as exc:
            problems.append({"file": rel, "reason": str(exc)})
            continue
        st = doc_stats(doc["lines"])
        totals["files"] += 1
        totals["lower" if path.endswith(SOG) else "upper"] += 1
        totals["bytes"] += st["bytes"]
        totals["records"] += st["records"]
        totals["blank"] += st["blank"]
        totals["blank_ws_only"] += st["blank_ws_only"]
        totals["outer_ws"] += st["outer_ws"]
        totals["names"] += st["names"]
        totals["dups"] += st["dups"]
        totals["dups_exact"] += st["dups_exact"]
        totals["dup_names"] += st["dup_names"]
        if st["dups"]:
            totals["dup_playlists"] += 1
        if not doc["final_eol"]:
            files_no_eol.append(rel)
        for line in doc["lines"]:
            core = line.strip()
            if core:
                name_lens.append(len(core.encode("latin1")))
            line_lens.append(len(line.encode("latin1")))
        folded.update(r["name"].lower() for r in record_rows(doc["lines"])
                      if r["name"])
    rep = {
        "tool": "sogwrite", "mode": "audit",
        "dir": os.path.abspath(directory),
        "files": totals["files"], "sog": totals["lower"],
        "SOG": totals["upper"], "bytes": totals["bytes"],
        "records": totals["records"], "blank": totals["blank"],
        "blank_ws_only": totals["blank_ws_only"],
        "outer_ws": totals["outer_ws"], "names": totals["names"],
        "dups": totals["dups"], "dups_exact": totals["dups_exact"],
        "dup_names": totals["dup_names"],
        "dup_playlists": totals["dup_playlists"],
        "distinct": len(folded),
        "name_len_min": min(name_lens) if name_lens else 0,
        "name_len_max": max(name_lens) if name_lens else 0,
        "line_len_min": min(line_lens) if line_lens else 0,
        "line_len_max": max(line_lens) if line_lens else 0,
        "without_final_eol": len(files_no_eol),
        "seconds": round(time.time() - started, 2),
        "problems": problems,
    }
    if len(name_lens) > 0:
        rep["name_len_histogram"] = dict(sorted(Counter(name_lens).items()))
    rep["ok"] = (not problems and totals["files"] == len(files))
    return rep


def print_audit(rep) -> None:
    print("audit: %s" % rep["dir"])
    print("  files=%d (%d .sog + %d .SOG)  bytes=%d  records=%d  names=%d"
          % (rep["files"], rep["sog"], rep["SOG"], rep["bytes"],
             rep["records"], rep["names"]))
    print("  blank=%d (whitespace-only %d)  outer-ws=%d  dups=%d "
          "(exact %d) in %d playlists, %d duplicate names"
          % (rep["blank"], rep["blank_ws_only"], rep["outer_ws"],
             rep["dups"], rep["dups_exact"], rep["dup_playlists"],
             rep["dup_names"]))
    print("  distinct(fold)=%d  name-len=%d..%d  line-len=%d..%d  "
          "without final CRLF=%d"
          % (rep["distinct"], rep["name_len_min"], rep["name_len_max"],
             rep["line_len_min"], rep["line_len_max"],
             rep["without_final_eol"]))
    hist = rep.get("name_len_histogram") or {}
    if hist:
        print("  name length histogram: %s"
              % ", ".join("%dB=%d" % (k, v) for k, v in hist.items()))
    if rep.get("seconds"):
        print("  %.2fs" % rep["seconds"])
    for problem in rep["problems"]:
        print("  problem: %s: %s" % (problem["file"], problem["reason"]))
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --set (one record, layout preserved)


def validate_name(name: str) -> dict:
    """Check a new sample name; returns warnings, raises on an impossible one."""
    if not name:
        raise WriteError("--name must not be empty (an empty line is not a "
                         "sample; blank records are not created here)")
    if name != name.strip():
        raise WriteError("--name %r has surrounding whitespace: the reader "
                         "trims records, so those bytes would never be part "
                         "of the name" % name)
    if "\r" in name or "\n" in name:
        raise WriteError("--name must not contain CR/LF: a record is one name")
    try:
        data = name.encode("latin1")
    except UnicodeEncodeError:
        raise WriteError("--name %r is not latin1-encodable; .sog is a "
                         "one-byte text file" % name)
    if b"\x00" in data:
        raise WriteError("--name must not contain a NUL byte")
    if len(data) > MAX_NAME_BYTES:
        raise WriteError("--name is %d bytes; the reader rejects names "
                         "longer than %d bytes (docs/PLAYLIST.md §2)"
                         % (len(data), MAX_NAME_BYTES))
    warnings = []
    if len(data) > ARCHIVE_NAME_BYTES:
        warnings.append(
            "name is %d bytes but a BIGFILE entry name is name[8]: the "
            "playlist will load and this record will never resolve "
            "(docs/PLAYLIST.md §2)" % len(data))
    if any(ch in SPACES for ch in name) or any(
            ord(ch) < 0x20 or ord(ch) == 0x7F for ch in name):
        warnings.append("name contains inner whitespace/control characters: "
                        "it will not match an archive entry")
    if any(ord(ch) > 0x7E for ch in name):
        warnings.append("name contains non-ASCII bytes (legal, compared "
                        "bytewise; absent from the shipped corpus)")
    return {"bytes": data, "warnings": warnings}


def set_name(path: str, line: int, name: str, out: str,
             strict: bool = False) -> dict:
    """Replace the name of record *line*; every other byte stays."""
    out = guard_out(out, path)
    doc = load(path)
    lines = doc["lines"]
    if line < 1 or line > len(lines):
        raise WriteError("--line %d is out of range: %s has %d record(s)"
                         % (line, path, len(lines)))
    valid = validate_name(name)
    if strict and valid["warnings"]:
        raise WriteError("--strict: %s" % valid["warnings"][0])
    old_line = lines[line - 1]
    lead = len(old_line) - len(old_line.lstrip(SPACES))
    trail = len(old_line) - len(old_line.rstrip(SPACES))
    was_blank = not old_line.strip()
    if was_blank:
        new_line = name
    else:
        core_end = len(old_line) - trail
        new_line = old_line[:lead] + name + old_line[core_end:]
    new_lines = list(lines)
    new_lines[line - 1] = new_line
    new_raw = to_bytes(new_lines)

    # proof 1: the record layout is untouched -- same count, same neighbours
    if len(new_lines) != len(lines):
        raise WriteError("internal: the edit changed the record count")
    if new_lines[:line - 1] != lines[:line - 1] or \
            new_lines[line:] != lines[line:]:
        raise WriteError("internal: an edit changed a neighbouring record")
    if new_raw.count(CRLF) != doc["raw"].count(CRLF):
        raise WriteError("internal: the edit changed the CRLF count")
    eol_flipped = new_raw.endswith(CRLF) != doc["raw"].endswith(CRLF)
    if eol_flipped and not (was_blank and line == len(lines)):
        raise WriteError("internal: the edit changed the final-EOL state")

    # proof 2: the parser sees exactly the new line, and gm/sog.py agrees
    redo = analyse(new_raw, out)
    if redo["lines"] != new_lines:
        raise WriteError("internal: re-reading the output does not give the "
                         "edited records")
    oracle = sogmod.parse(new_raw)
    if oracle.lines != new_lines:
        raise WriteError("internal: gm/sog.py splits the output differently")
    if name not in oracle.names:
        raise WriteError("internal: gm/sog.py does not see the new name")

    # proof 3: the output passes --check (no structural problems)
    verdict = doc_problems(redo, out)
    if verdict["problems"]:
        raise WriteError("internal: the edited file does not pass --check: %s"
                         % "; ".join(verdict["problems"]))

    stat_line = diff_stats(old_line.encode("latin1"), new_line.encode("latin1"))
    stat_file = diff_stats(doc["raw"], new_raw)
    warnings = list(valid["warnings"])
    if eol_flipped:
        warnings.append(
            "record %d is the empty record after the final CRLF (--list shows "
            "it); it now carries the name, so the file no longer ends with "
            "CRLF (still %d records, no line added)"
            % (line, len(lines)))
    rep = {
        "tool": "sogwrite", "mode": "set", "file": path,
        "out": out, "line": line, "records": len(lines),
        "old_name": old_line.strip(), "new_name": name,
        "old_line": old_line, "new_line": new_line,
        "line_was_blank": was_blank,
        "lead_spaces": lead, "trail_spaces": trail,
        "bytes_in": len(doc["raw"]), "bytes_out": len(new_raw),
        "delta": len(new_raw) - len(doc["raw"]),
        "changed_bytes": stat_line["changed"],
        "line_delta": stat_line["delta"],
        "first_diff": stat_line["first_diff"],
        "file_first_diff": stat_file["first_diff"],
        "file_changed_bytes": stat_file["changed"],
        "sha256_in": sha256(doc["raw"]), "sha256_out": sha256(new_raw),
        "layout_same": True,
        "oracle_names": len(oracle.names),
        "warnings": warnings,
        "check_ok": True,
        "check_warnings": verdict["warnings"],
        "final_eol": redo["final_eol"],
    }
    write_bytes(out, new_raw)
    if sha256(read_bytes(out)) != rep["sha256_out"]:
        raise WriteError("internal: %s on disk does not match the report" % out)
    return rep


def print_set(rep) -> None:
    print("edit: %s -> %s (record %d/%d)"
          % (rep["file"], rep["out"], rep["line"], rep["records"]))
    print("  name: %r -> %r%s"
          % (rep["old_name"], rep["new_name"],
             " (the record was blank: a song gains a track, no line added)"
             if rep["line_was_blank"] else ""))
    print("  bytes: %d -> %d (%+d); changed %d byte(s) in the record "
          "(first at offset %s inside the line), layout CRLF/spaces preserved"
          % (rep["bytes_in"], rep["bytes_out"], rep["delta"],
             rep["changed_bytes"],
             rep["first_diff"] if rep["first_diff"] is not None else "n/a"))
    print("  file diff (positional, a length change shifts the tail): "
          "%d byte(s), first at offset %s"
          % (rep["file_changed_bytes"], rep["file_first_diff"]))
    print("  sha256: %s -> %s"
          % (rep["sha256_in"][:16], rep["sha256_out"][:16]))
    print("  gm/sog.py sees %d name(s); --check: %s"
          % (rep["oracle_names"], "ok" if rep["check_ok"] else "FAILED"))
    for warning in rep["warnings"]:
        print("  warning: %s" % warning)


# ---------------------------------------------------------------------------
# --roundtrip


def roundtrip_corpus(directory: str = "", verbose: bool = False) -> dict:
    if not directory:
        directory = os.path.join(GAME, "MUSIC")
    if not os.path.isdir(directory):
        raise WriteError("%s is not a directory" % directory)
    files = sog_files(directory)
    if not files:
        raise WriteError("no .sog files under %s" % directory)

    started = time.time()
    rows = []
    counts = Counter()
    totals = {"bytes": 0}
    for path in files:
        rel = os.path.relpath(path, directory).replace(os.sep, "/")
        raw = read_bytes(path)
        totals["bytes"] += len(raw)
        row = {"file": rel, "bytes": len(raw), "problems": []}
        try:
            doc = analyse(raw, rel)
        except WriteError as exc:
            counts["error"] += 1
            row["status"] = "error"
            row["reason"] = str(exc)
            rows.append(row)
            continue
        out = to_bytes(doc["lines"])
        if out == raw:
            counts["identical"] += 1
            row["status"] = "identical"
        else:
            counts["differs"] += 1
            stat = diff_stats(raw, out)
            row["status"] = "differs"
            row["reason"] = ("re-serialised %d B != %d B; %d byte(s) differ, "
                             "first at offset %s"
                             % (len(out), len(raw), stat["changed"],
                                stat["first_diff"]))
            row["problems"].append(row["reason"])
        oracle = sogmod.parse(raw)
        if oracle.to_bytes() == raw:
            counts["oracle_bytes"] += 1
        else:
            row["problems"].append("gm/sog.py to_bytes != file")
        if oracle.names == doc["names"]:
            counts["oracle_names"] += 1
        else:
            row["problems"].append("gm/sog.py names differ")
        row["records"] = len(doc["lines"])
        row["names"] = len(doc["names"])
        row["final_eol"] = doc["final_eol"]
        rows.append(row)
        if verbose:
            print("  %-24s %-9s %5d B  %3d records, %3d names"
                  % (rel, row["status"], len(raw), row["records"],
                     row["names"]))
    rep = {
        "tool": "sogwrite", "mode": "roundtrip",
        "dir": os.path.abspath(directory),
        "files": len(files),
        "sog": sum(1 for p in files if p.endswith(SOG)),
        "SOG": sum(1 for p in files if p.endswith(".SOG")),
        "identical": counts["identical"], "differs": counts["differs"],
        "errors": counts["error"], "bytes": totals["bytes"],
        "oracle_identical": counts["oracle_bytes"],
        "oracle_names_match": counts["oracle_names"],
        "seconds": round(time.time() - started, 2),
        "results": rows,
    }
    rep["ok"] = (counts["identical"] == len(files)
                 and not counts["differs"] and not counts["error"]
                 and counts["oracle_bytes"] == len(files)
                 and counts["oracle_names"] == len(files))
    return rep


def print_roundtrip(rep) -> None:
    print("roundtrip: %s" % rep["dir"])
    print("  %d files (%d .sog + %d .SOG), %d bytes, %.2fs"
          % (rep["files"], rep["sog"], rep["SOG"], rep["bytes"],
             rep["seconds"]))
    print("  identical=%d/%d  differs=%d  errors=%d"
          % (rep["identical"], rep["files"], rep["differs"], rep["errors"]))
    print("  oracle gm/sog.py: to_bytes identical=%d/%d, names match=%d/%d"
          % (rep["oracle_identical"], rep["files"],
             rep["oracle_names_match"], rep["files"]))
    for row in rep["results"]:
        if row["status"] != "identical":
            print("  %-24s %-9s %s"
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
        base = "/tmp/opencode"
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = None
    return tempfile.mkdtemp(prefix="sogwrite-selftest-", dir=base)


def _selftest() -> bool:
    results = []
    work = _selftest_workdir()
    good = b"Dm1A01\r\nDm1A02\r\nDm1A03\r\n\r\n"

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
            return "refused: %s" % str(exc)[:60]
        raise AssertionError("expected WriteError containing %r" % needle)

    def path(name, data=None):
        p = os.path.join(work, name)
        if data is not None:
            os.makedirs(os.path.dirname(p) or work, exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        return p

    def t_split_join():
        assert to_bytes(analyse(good)["lines"]) == good
        return "%d bytes, %d records" % (len(good), 4)

    def t_no_final_eol():
        raw = b"L01C01 \r\nL01C02"
        assert to_bytes(analyse(raw)["lines"]) == raw
        return "no final CRLF preserved"

    def t_trailing_spaces():
        raw = b"L01C01 \r\nL01C02 \r\n"
        assert to_bytes(analyse(raw)["lines"]) == raw
        return "outer spaces preserved"

    def t_trailing_blank_record():
        doc = analyse(b"winner\r\n\r\n")
        assert doc["lines"] == ["winner", "", ""]
        assert to_bytes(doc["lines"]) == b"winner\r\n\r\n"
        return "two trailing records"

    def t_names_trim():
        doc = analyse(b"  A1 \r\n\r\n  B2\r\n")
        assert doc["names"] == ["A1", "B2"], doc["names"]
        return "trimmed names"

    def t_lone_lf():
        return expect_error(lambda: analyse(b"A\r\nB\n"), "lone CR/LF")

    def t_lone_cr():
        return expect_error(lambda: analyse(b"A\rB\r\n"), "lone CR/LF")

    def t_empty_refused():
        return expect_error(lambda: analyse(b""), "empty playlist")

    def t_extension_refused():
        return expect_error(lambda: kind_of(path("x.txt", good)), "not a .sog")

    def t_upper_extension_ok():
        assert kind_of("A.SOG") == ".SOG"
        return ".SOG accepted"

    def t_latin1_roundtrip():
        raw = "caf\u00e9\r\nna\u00efve\r\n".encode("latin1")
        assert to_bytes(analyse(raw)["lines"]) == raw
        return "high bytes are 1 byte/char"

    def t_check_ok():
        rep = check_file(path("ok.sog", good))
        assert rep["ok"] and not rep["problems"], rep["problems"]
        assert rep["stats"]["names"] == 3 and rep["stats"]["blank"] == 2
        return "structure ok, blank record is data"

    def t_check_empty_code2():
        return expect_error(lambda: check_file(path("empty.sog", b"")),
                            "empty playlist")

    def t_check_whitespace_only():
        rep = check_file(path("ws.sog", b"   \r\n\r\n"))
        assert not rep["ok"] and "no sample names" in rep["problems"][0]
        return "whitespace-only playlist is a problem"

    def t_check_long_name_warns():
        rep = check_file(path("long.sog", b"Dm1A012345\r\n"))
        assert rep["ok"], rep["problems"]
        assert any("name[8]" in w for w in rep["warnings"]), rep["warnings"]
        return "9-byte name: warning, not an error"

    def t_check_against_identical():
        a = path("same.sog", good)
        rep = check_file(a, a)
        assert rep["identical"] and rep["changed_bytes"] == 0
        return "identical against itself"

    def t_check_against_diff():
        a = path("orig.sog", good)
        b = path("edit.sog", b"Dm1A01\r\nDm1A02\r\nDm1A99\r\n\r\n")
        rep = check_file(b, a)
        assert rep["structure_ok"] and not rep["identical"]
        assert rep["changed_bytes"] == 2, rep["changed_bytes"]
        assert rep["first_changed_record"]["index"] == 3
        return "changed_bytes=2, record 3"

    def t_list_rows():
        rep = list_records(path("list.sog", b" A \r\nB\r\n"))
        assert [r["index"] for r in rep["rows"]] == [1, 2, 3]
        assert rep["rows"][0]["name"] == "A" and "outer-ws" in \
            rep["rows"][0]["flags"]
        assert rep["rows"][2]["name"] == "" and "blank" in \
            rep["rows"][2]["flags"]
        return "%d rows" % len(rep["rows"])

    def t_set_one_line():
        src = path("edit-in.sog", good)
        dst = path("edit-out.sog")
        rep = set_name(src, 2, "Dm1A09", dst)
        out = read_bytes(dst)
        assert out == b"Dm1A01\r\nDm1A09\r\nDm1A03\r\n\r\n", out
        assert rep["changed_bytes"] == 1 and rep["delta"] == 0
        assert rep["layout_same"] and rep["check_ok"]
        return "changed %d byte(s)" % rep["changed_bytes"]

    def t_set_keeps_spaces():
        src = path("sp-in.sog", b"L01C01 \r\nL01C02 \r\n")
        dst = path("sp-out.sog")
        rep = set_name(src, 1, "L01C09", dst)
        assert read_bytes(dst) == b"L01C09 \r\nL01C02 \r\n"
        assert rep["trail_spaces"] == 1
        return "trailing space kept"

    def t_set_keeps_no_eol():
        src = path("eol-in.sog", b"L01D01\r\nL01D02")
        dst = path("eol-out.sog")
        rep = set_name(src, 2, "L01D09", dst)
        assert read_bytes(dst) == b"L01D01\r\nL01D09"
        assert rep["final_eol"] is False
        return "no final CRLF kept"

    def t_set_len_change():
        src = path("len-in.sog", b"AAA\r\nBBB\r\n")
        dst = path("len-out.sog")
        rep = set_name(src, 1, "LONGGG", dst)
        assert read_bytes(dst) == b"LONGGG\r\nBBB\r\n"
        assert rep["delta"] == 3 and rep["changed_bytes"] == 6
        return "delta +3, 6 positional byte(s) in the record"

    def t_set_blank_record():
        src = path("blank-in.sog", b"AAA\r\n\r\n")
        dst = path("blank-out.sog")
        rep = set_name(src, 2, "BBB", dst)
        assert read_bytes(dst) == b"AAA\r\nBBB\r\n"
        assert rep["line_was_blank"] and rep["records"] == 3
        return "blank record filled, no line added"

    def t_set_out_of_range():
        src = path("range.sog", good)
        return expect_error(
            lambda: set_name(src, 9, "X1", path("range-out.sog")), "out of range")

    def t_set_line_zero():
        src = path("zero.sog", good)
        return expect_error(
            lambda: set_name(src, 0, "X1", path("zero-out.sog")), "out of range")

    def t_set_empty_name():
        src = path("nm0.sog", good)
        return expect_error(
            lambda: set_name(src, 1, "", path("nm0-out.sog")), "must not be empty")

    def t_set_crlf_name():
        src = path("nmr.sog", good)
        return expect_error(
            lambda: set_name(src, 1, "A\r\nB", path("nmr-out.sog")), "CR/LF")

    def t_set_space_name():
        src = path("nms.sog", good)
        return expect_error(
            lambda: set_name(src, 1, " AB", path("nms-out.sog")),
            "surrounding whitespace")

    def t_set_long_name_warns():
        src = path("nml.sog", good)
        rep = set_name(src, 1, "ABCDEFGHI", path("nml-out.sog"))
        assert rep["warnings"] and "name[8]" in rep["warnings"][0]
        return "9-byte name: warning"

    def t_set_strict_long():
        src = path("nml2.sog", good)
        return expect_error(
            lambda: set_name(src, 1, "ABCDEFGHI", path("nml2-out.sog"),
                             strict=True), "--strict")

    def t_set_name_too_long():
        src = path("nm256.sog", good)
        return expect_error(
            lambda: set_name(src, 1, "A" * 256, path("nm256-out.sog")),
            "255 bytes")

    def t_set_missing_file():
        return expect_error(lambda: set_name(path("nope.sog"), 1, "A",
                                             path("nope-out.sog")), "cannot read")

    def t_guard_game():
        return expect_error(
            lambda: guard_out(os.path.join(DEFAULT_GAME, "MUSIC", "x.sog")),
            "refusing to write")

    def t_guard_mirror():
        return expect_error(
            lambda: guard_out(os.path.join(MIRROR_GAME, "MUSIC", "x.sog")),
            "refusing to write")

    def t_guard_repo():
        return expect_error(
            lambda: guard_out(os.path.join(ROOT, "MUSIC", "x.sog")),
            "refusing to write")

    def t_guard_same_file():
        src = path("same-in.sog", good)
        return expect_error(lambda: guard_out(src, src), "refusing to overwrite")

    def t_audit_synth():
        d = os.path.join(work, "corpus")
        os.makedirs(os.path.join(d, "sub"), exist_ok=True)
        with open(os.path.join(d, "a.sog"), "wb") as f:
            f.write(b"AAA\r\nAAA \r\n\r\n")
        with open(os.path.join(d, "sub", "b.SOG"), "wb") as f:
            f.write(b"BBB\r\n")
        rep = audit_corpus(d)
        assert rep["files"] == 2 and rep["sog"] == 1 and rep["SOG"] == 1
        assert rep["records"] == 6 and rep["blank"] == 3
        assert rep["names"] == 3 and rep["dups"] == 1
        assert rep["outer_ws"] == 1 and rep["without_final_eol"] == 0
        assert rep["name_len_min"] == 3 and rep["name_len_max"] == 3
        return "files=2 records=6 names=3 dups=1"

    def t_roundtrip_synth():
        d = os.path.join(work, "rt")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "a.sog"), "wb") as f:
            f.write(b"AAA \r\n\r\nBBB")
        with open(os.path.join(d, "b.SOG"), "wb") as f:
            f.write(b"BBB\r\n")
        rep = roundtrip_corpus(d)
        assert rep["ok"], rep["results"]
        assert rep["identical"] == 2 and rep["oracle_identical"] == 2
        return "identical=2/2, oracle=2/2"

    def t_roundtrip_refuses_lf():
        d = os.path.join(work, "rt-lf")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "a.sog"), "wb") as f:
            f.write(b"AAA\nBBB\n")
        rep = roundtrip_corpus(d)
        assert not rep["ok"] and rep["errors"] == 1, rep
        assert "lone CR/LF" in rep["results"][0]["reason"]
        return "LF-only file reported, not silently normalised"

    def t_sha_and_diff():
        assert diff_stats(b"abc", b"axc") == {"changed": 1, "first_diff": 1,
                                              "delta": 0}
        assert diff_stats(b"abc", b"ab") == {"changed": 1, "first_diff": 2,
                                             "delta": -1}
        assert sha256(b"") == \
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        return "diff/sha helpers"

    def t_esc():
        assert esc("A\x00\xff") == "A\\x00\\xff"
        assert esc("A\tB") == "A\\tB"
        return "byte-faithful display"

    for name, fn in [
            ("split/join identity", t_split_join),
            ("last line without CRLF", t_no_final_eol),
            ("trailing spaces", t_trailing_spaces),
            ("trailing blank record", t_trailing_blank_record),
            ("names are trimmed", t_names_trim),
            ("lone LF refused", t_lone_lf),
            ("lone CR refused", t_lone_cr),
            ("empty file refused", t_empty_refused),
            ("extension refused", t_extension_refused),
            (".SOG accepted", t_upper_extension_ok),
            ("latin1 is 1 byte/char", t_latin1_roundtrip),
            ("check: structure ok", t_check_ok),
            ("check: empty -> code 2", t_check_empty_code2),
            ("check: whitespace-only", t_check_whitespace_only),
            ("check: long name warns", t_check_long_name_warns),
            ("check: --against identical", t_check_against_identical),
            ("check: --against diff", t_check_against_diff),
            ("list: rows/flags", t_list_rows),
            ("set: one record", t_set_one_line),
            ("set: keeps spaces", t_set_keeps_spaces),
            ("set: keeps missing EOL", t_set_keeps_no_eol),
            ("set: length change", t_set_len_change),
            ("set: blank record filled", t_set_blank_record),
            ("set: line out of range", t_set_out_of_range),
            ("set: line 0", t_set_line_zero),
            ("set: empty name", t_set_empty_name),
            ("set: CR/LF name", t_set_crlf_name),
            ("set: space name", t_set_space_name),
            ("set: long name warns", t_set_long_name_warns),
            ("set: --strict refuses", t_set_strict_long),
            ("set: name > 255 B", t_set_name_too_long),
            ("set: missing file", t_set_missing_file),
            ("guard: game", t_guard_game),
            ("guard: mirror", t_guard_mirror),
            ("guard: repository", t_guard_repo),
            ("guard: same file", t_guard_same_file),
            ("audit: synthetic corpus", t_audit_synth),
            ("roundtrip: synthetic", t_roundtrip_synth),
            ("roundtrip: LF file", t_roundtrip_refuses_lf),
            ("helpers: sha/diff", t_sha_and_diff),
            ("helpers: escaping", t_esc),
    ]:
        case(name, fn)

    shutil.rmtree(work, ignore_errors=True)
    passed = sum(1 for _n, ok, _d in results if ok)
    for name, ok, detail in results:
        print("  %-30s %s  %s" % (name, "ok  " if ok else "FAIL", detail))
    print("self-test: %d/%d" % (passed, len(results)))
    return passed == len(results)


# ---------------------------------------------------------------------------
# CLI


def _cmd_roundtrip(args) -> int:
    rep = roundtrip_corpus(args.roundtrip or "", args.verbose)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_roundtrip(rep)
    return 0 if rep["ok"] else 1


def _cmd_audit(args) -> int:
    rep = audit_corpus(args.audit or "")
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_audit(rep)
    return 0 if rep["ok"] else 1


def _cmd_list(args) -> int:
    rep = list_records(args.list)
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


def _cmd_set(args) -> int:
    if args.line is None:
        raise WriteError("--set needs --line N (a 1-based record)")
    if args.name is None:
        raise WriteError("--set needs --name NAME")
    if not args.out:
        raise WriteError("--set needs --out OUT.sog")
    rep = set_name(args.set, args.line, args.name, args.out, args.strict)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_set(rep)
    return 0 if rep["check_ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(
        description="Writer/editor for the Gunmetal .sog song playlists "
                    "(M11.5).")
    ap.add_argument("--roundtrip", nargs="?", const="", metavar="DIR",
                    help="split/re-join every .sog and compare byte for byte "
                         "(default game/MUSIC, recursive)")
    ap.add_argument("--audit", nargs="?", const="", metavar="DIR",
                    help="numbers of the whole corpus (default game/MUSIC)")
    ap.add_argument("--list", metavar="FILE",
                    help="list records with index, offset, length and flags")
    ap.add_argument("--check", metavar="FILE",
                    help="structural check of one .sog")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: also compare byte for byte with FILE")
    ap.add_argument("--set", metavar="FILE",
                    help="replace the name of one record, keeping layout")
    ap.add_argument("--line", type=int, metavar="N",
                    help="1-based record to edit (as --list prints it)")
    ap.add_argument("--name", metavar="NAME",
                    help="the new sample name (latin1, no CR/LF, <=255 B)")
    ap.add_argument("--out", metavar="PATH", help="output file")
    ap.add_argument("--strict", action="store_true",
                    help="--set: refuse a name that cannot resolve "
                         "(longer than 8 bytes, inner spaces, non-ASCII)")
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
        return 0 if _selftest() else 1
    chosen = [x for x in (args.roundtrip, args.audit, args.list, args.check,
                          args.set) if x is not None]
    if len(chosen) > 1:
        ap.error("choose one of --roundtrip/--audit/--list/--check/--set")
    if args.roundtrip is not None:
        return _cmd_roundtrip(args)
    if args.audit is not None:
        return _cmd_audit(args)
    if args.list:
        return _cmd_list(args)
    if args.check:
        return _cmd_check(args)
    if args.set:
        return _cmd_set(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except WriteError as exc:
        print("sogwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, OSError) as exc:
        print("sogwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                           # noqa: BLE001
        print("sogwrite: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
