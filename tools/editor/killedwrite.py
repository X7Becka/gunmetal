#!/usr/bin/env python3
"""Layout-preserving writer/editor for the Gunmetal kill-message tables
(``FRONTEND/KILLED.skb`` / ``KILLED.skk``, ``M11.5``, slice 12).

Both files are DOS **text**: one kill message per line, CRLF endings, no header,
no offsets, no checksum (``docs/FORMATS.md`` §8.2, ``docs/KILLED.md``).  The
engine opens them with ``fopen(..., "r")`` (`0x485a00` / `0x485b2f`), reads them
with ``fgets(buf, 0x400, f)`` and keeps a line only when
``0 < strlen(line) < 0x20`` (`0x485a7d`).  Text mode folds CRLF to LF and
``fgets`` keeps the ``\\n``, so the *visible* text of a kept message is **1..30
bytes**; both shipped files end with an empty CRLF line and hold 69 (`.skb`) and
50 (`.skk`) kept messages.

Because the format is plain text, the writer is a line model, not a binary
splice::

    records = raw.split(b"\\r\\n")   # the element after the final CRLF is dropped
    raw     = CRLF.join(records) + (CRLF if final_eol else b"")

That is the identity for every shipped file (2/2, 4 305 bytes): CRLF only, one
trailing empty record, no NUL, pure ASCII.  A file with a lone ``CR``/``LF`` is
**refused**, never silently normalised, because the split/join pair would not be
inverse.

What the tool can do
--------------------
``--roundtrip [DIR|FILE]``  read and re-serialise every ``.skb``/``.skk``; compare
                            byte for byte and against ``gm/small.py``.
``--audit [DIR|FILE]``      per-file numbers (bytes, lines, filter, lengths,
                            placeholders, duplicates, EOL).
``--list FILE [--accepted]`` every record with its index, "kept by the engine"
                            mark and placeholder count.
``--check FILE [--against ORIG]``
                            structure (EOL, trailing empty CRLF, encoding) plus a
                            byte comparison.
``--set LINE --text TEXT --out OUT``
                            replace the text of one record; every other byte
                            (CRLF, the trailing empty line, neighbours) stays.
``--prepend LINE --text TEXT --out OUT`` / ``--append --text TEXT --out OUT``
                            add one record without breaking the tail.
``--self-test``             synthetic checks, no game files.

The reader/writer contract is the C++ module ``ui/killed.{h,cpp}``
(``docs/KILLED.md`` §3): 30 visible bytes, 128 messages, ``%S``/``%s`` are the
same placeholder.  Only the *text* is edited here -- nothing is drawn, no game
code and no C++ file is touched.

Exit codes: ``0`` success, ``1`` verification failed, ``2`` bad input or a
refused edit (message on stderr), ``3`` internal error.

Refusals (code 2): not ``.skb``/``.skk``; an empty file; a lone ``CR``/``LF``;
a NUL byte (``strlen`` would cut the message); more than two placeholders on one
line (the templates carry at most the killer and the victim); non-ASCII or
control bytes in ``--text``; a line longer than 1022 bytes (``fgets`` would split
it into several messages); ``--out`` inside the game installation, its mirror or
either repository checkout, or ``--out`` equal to the input.
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

from gm import small as smallmod                  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")
#: the primary checkout: this file may live in a git worktree under ~/build, and
#: neither root is a legal output target
MAIN_REPO = os.environ.get("GUNMETAL_REPO", "/home/nooverload/gunmetal-re")
#: the game installation in use (``--game`` overrides it)
GAME = DEFAULT_GAME

FRONTEND = "FRONTEND"
EXTENSIONS = (".skb", ".skk")
CRLF = b"\r\n"

#: ``0x485a7d``: ``strlen(line) < 0x20`` counts the ``\n`` of ``fgets``, so a
#: kept message has 1..30 visible bytes
MAX_LINE_BYTES = 30
#: ``0x68ddb8`` / ``0x68ddb4``: at most 128 messages per array
MAX_MESSAGES = 0x80
#: model cap of the reader (``docs/KILLED.md`` §2.7)
MAX_FILE_BYTES = 64 * 1024
#: ``fgets(buf, 0x400, f)`` reads at most 1023 bytes *including* the terminator:
#: a longer line arrives as several chunks, i.e. as extra messages
MAX_TEXT_BYTES = 1022
#: ``%S KILLS %S.`` -- the templates name at most the killer and the victim
MAX_PLACEHOLDERS = 2
#: a line of nothing but these bytes prints nothing (``docs/KILLED.md`` §2.5)
SPACES = " \t"


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
    """The reference installation, its mirror and both repository checkouts."""
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


def kind_of(path: str) -> str:
    ext = os.path.splitext(path)[1]
    if ext.lower() not in EXTENSIONS:
        raise WriteError("%s: not a .skb/.skk kill table (extension %r)"
                         % (path, ext or "<none>"))
    return ext.lower()


def killed_files(directory: str) -> list:
    """Every ``.skb``/``.skk`` under *directory* (recursive, case-insensitive)."""
    out = []
    for base, _dirs, names in os.walk(directory):
        for name in names:
            if os.path.splitext(name)[1].lower() in EXTENSIONS:
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


def count_placeholders(text: str) -> tuple:
    """``(%S, %s)`` counts, scanned exactly like ``ui/killed.cpp``."""
    upper = lower = 0
    i = 0
    n = len(text)
    while i + 1 < n:
        if text[i] == "%":
            nxt = text[i + 1]
            if nxt == "S":
                upper += 1
                i += 2
                continue
            if nxt == "s":
                lower += 1
                i += 2
                continue
        i += 1
    return upper, lower


def encoding_of(raw: bytes) -> dict:
    high = sorted({b for b in raw if b > 0x7F})
    return {"ascii": not high, "latin1": True, "high_bytes": high}


# ---------------------------------------------------------------------------
# document model (a line list; ``gm/small.py::parse_kill`` is the oracle)


def to_bytes(records, final_eol: bool) -> bytes:
    """Serialise records with CRLF; the final CRLF is added only if it was there."""
    data = CRLF.join(rec.encode("latin1") for rec in records)
    return data + CRLF if final_eol and records else data


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
    """Split a kill table into exact records; refuse what the model cannot hold."""
    name = where or "<bytes>"
    if not raw:
        raise WriteError("empty file: %s (both shipped kill tables are text, "
                         "1967 and 2338 bytes)" % name)
    if len(raw) > MAX_FILE_BYTES:
        raise WriteError(
            "%s is %d bytes, larger than %d: the reader refuses a file that big "
            "(the shipped files are 1967 and 2338)"
            % (name, len(raw), MAX_FILE_BYTES))
    nul = raw.count(b"\x00")
    if nul:
        raise WriteError(
            "%s has %d NUL byte(s): the engine measures a message with strlen, "
            "so a NUL would silently cut the line" % (name, nul))
    bad = _lone_eol_offset(raw)
    if bad is not None:
        raise WriteError(
            "%s: byte 0x%02x at offset %d is a lone CR/LF; only CRLF records are "
            "modelled (both shipped files are pure CRLF)"
            % (name, raw[bad], bad))
    final_eol = raw.endswith(CRLF)
    records = raw.decode("latin1").split("\r\n")
    if final_eol:
        records.pop()
    out = to_bytes(records, final_eol)
    if out != raw:
        raise WriteError("internal: %s does not round-trip through the line "
                         "model (%d B vs %d B)" % (name, len(out), len(raw)))
    return {
        "raw": raw,
        "bytes": len(raw),
        "records": records,
        "lines": len(records),
        "final_eol": final_eol,
        "trailing_blank": bool(records) and records[-1] == "" and final_eol,
        "crlf": raw.count(CRLF),
        "encoding": encoding_of(raw),
    }


def load(path: str) -> dict:
    kind_of(path)
    doc = analyse(read_bytes(path), path)
    doc["path"] = path
    return doc


def doc_stats(records) -> dict:
    """Per-file numbers; they are *data* (duplicates, long lines), not errors."""
    lengths = [len(rec.encode("latin1")) for rec in records]
    accepted = [rec for rec in records
                if 0 < len(rec.encode("latin1")) <= MAX_LINE_BYTES]
    non_empty = [rec for rec in records if rec]
    blank = [rec for rec in records if rec and not rec.strip(SPACES)]
    ph_upper = sum(count_placeholders(rec)[0] for rec in records)
    ph_lower = sum(count_placeholders(rec)[1] for rec in records)
    ph_accepted = sum(count_placeholders(rec)[0] + count_placeholders(rec)[1]
                      for rec in accepted)
    per_line = [count_placeholders(rec)[0] + count_placeholders(rec)[1]
                for rec in records]
    exact = Counter(non_empty)
    folded = Counter(rec.lower() for rec in non_empty)
    nonempty_lens = [len(rec.encode("latin1")) for rec in non_empty]
    accepted_lens = [len(rec.encode("latin1")) for rec in accepted]
    return {
        "lines": len(records),
        "non_empty": len(non_empty),
        "blank": len(blank),
        "accepted": len(accepted),
        "accepted_blank": sum(1 for rec in accepted if not rec.strip(SPACES)),
        "rejected_long": sum(1 for n in lengths if n > MAX_LINE_BYTES),
        "stored": min(len(accepted), MAX_MESSAGES),
        "over_limit": max(0, len(accepted) - MAX_MESSAGES),
        "line_len_min": min(lengths) if lengths else 0,
        "line_len_max": max(lengths) if lengths else 0,
        "nonempty_len_min": min(nonempty_lens) if nonempty_lens else 0,
        "nonempty_len_max": max(nonempty_lens) if nonempty_lens else 0,
        "accepted_len_min": min(accepted_lens) if accepted_lens else 0,
        "accepted_len_max": max(accepted_lens) if accepted_lens else 0,
        "ph_upper": ph_upper,
        "ph_lower": ph_lower,
        "ph_total": ph_upper + ph_lower,
        "ph_accepted": ph_accepted,
        "ph_max_line": max(per_line) if per_line else 0,
        "dups": sum(v - 1 for v in exact.values()),
        "dup_lines": sum(1 for v in exact.values() if v > 1),
        "dups_fold": sum(v - 1 for v in folded.values()),
        "dup_lines_fold": sum(1 for v in folded.values() if v > 1),
        "distinct_fold": len(folded),
    }


def line_rows(doc: dict) -> list:
    """One row per record: index, byte offset, length, flags, placeholders."""
    rows = []
    offset = 0
    exact = Counter(rec for rec in doc["records"] if rec)
    seen = Counter()
    for i, rec in enumerate(doc["records"]):
        data = rec.encode("latin1")
        length = len(data)
        upper, lower = count_placeholders(rec)
        flags = []
        if not rec:
            flags.append("blank")
        elif not rec.strip(SPACES):
            flags.append("ws-only")
        if length > MAX_LINE_BYTES:
            flags.append("long")
        accepted = 0 < length <= MAX_LINE_BYTES
        if accepted:
            flags.append("accepted")
        if upper + lower > MAX_PLACEHOLDERS:
            flags.append("ph>%d" % MAX_PLACEHOLDERS)
        if rec:
            seen[rec] += 1
            if exact[rec] > 1:
                flags.append("dup%d" % seen[rec])
        rows.append({"index": i + 1, "offset": offset, "length": length,
                     "placeholders": upper + lower, "upper": upper,
                     "lower": lower, "accepted": accepted, "text": rec,
                     "flags": flags})
        offset += length + len(CRLF)
    return rows


# ---------------------------------------------------------------------------
# --check


def doc_problems(doc: dict, where: str) -> dict:
    """Structural verdict: problems (refuse) vs notes/warnings (data)."""
    problems, warnings = [], []
    st = doc_stats(doc["records"])
    if st["accepted"] == 0:
        problems.append(
            "no message passes the engine filter (0 < strlen < 0x20, i.e. "
            "1..%d visible bytes): the reader would refuse the file" % MAX_LINE_BYTES)
    if not doc["final_eol"]:
        warnings.append("the file does not end with CRLF (both shipped files do)")
    elif not doc["trailing_blank"]:
        warnings.append("the last record is not empty: both shipped files end "
                        "with an empty CRLF line (CRLF CRLF)")
    if st["rejected_long"]:
        first = next(r for r in line_rows(doc) if "long" in r["flags"])
        warnings.append(
            "%d line(s) longer than %d bytes are dropped by the engine "
            "(first: line %d, %d B)"
            % (st["rejected_long"], MAX_LINE_BYTES, first["index"], first["length"]))
    if st["dups"]:
        warnings.append(
            "%d duplicate occurrence(s) in %d distinct line(s) -- data, kept "
            "as-is" % (st["dups"], st["dup_lines"]))
    if st["over_limit"]:
        warnings.append(
            "%d line(s) pass the filter but the engine stores at most %d "
            "messages (0x68ddb8/0x68ddb4)" % (st["accepted"], MAX_MESSAGES))
    if not doc["encoding"]["ascii"]:
        warnings.append(
            "non-ASCII byte(s) %s: latin-1 is legal for the reader (bytes are "
            "compared bytewise) but absent from the shipped corpus; --set/"
            "--prepend/--append refuse such a file"
            % ", ".join("0x%02x" % b for b in doc["encoding"]["high_bytes"][:8]))
    return {"problems": problems, "warnings": warnings, "stats": st,
            "rows": line_rows(doc)}


def check_file(path: str, against: str | None = None) -> dict:
    doc = load(path)
    verdict = doc_problems(doc, path)
    st = verdict["stats"]
    rep = {
        "tool": "killedwrite", "mode": "check", "file": os.path.abspath(path),
        "kind": os.path.splitext(path)[1].lower(),
        "bytes": doc["bytes"], "lines": doc["lines"],
        "final_eol": doc["final_eol"],
        "trailing_blank": doc["trailing_blank"],
        "crlf": doc["crlf"], "lone_eol": 0,
        "encoding": doc["encoding"]["ascii"] and "ascii" or "latin-1",
        "ascii": doc["encoding"]["ascii"],
        "high_bytes": doc["encoding"]["high_bytes"],
        "sha256": sha256(doc["raw"]),
        "stats": st,
        "problems": verdict["problems"], "warnings": verdict["warnings"],
    }
    rep["structure_ok"] = not verdict["problems"]
    rep["ok"] = rep["structure_ok"]
    if against:
        other = load(against)
        st_other = doc_stats(other["records"])
        cmp = diff_stats(other["raw"], doc["raw"])
        first = None
        for i in range(min(len(doc["records"]), len(other["records"]))):
            if doc["records"][i] != other["records"][i]:
                first = {"index": i + 1,
                         "before": other["records"][i],
                         "after": doc["records"][i]}
                break
        if first is None and len(doc["records"]) != len(other["records"]):
            first = {"index": min(len(doc["records"]), len(other["records"])) + 1,
                     "before": "<%d lines>" % len(other["records"]),
                     "after": "<%d lines>" % len(doc["records"])}
        rep["against"] = os.path.abspath(against)
        rep["against_bytes"] = other["bytes"]
        rep["against_sha256"] = sha256(other["raw"])
        rep["identical"] = doc["raw"] == other["raw"]
        rep["changed_bytes"] = cmp["changed"]
        rep["first_diff"] = cmp["first_diff"]
        rep["delta"] = cmp["delta"]
        rep["lines_before"] = len(other["records"])
        rep["accepted_before"] = st_other["accepted"]
        rep["ph_accepted_before"] = st_other["ph_accepted"]
        rep["first_changed_line"] = first
    return rep


def print_check(rep) -> None:
    print("check: %s" % rep["file"])
    print("  %d bytes, %d lines, final CRLF=%s, final empty line=%s, sha256=%s"
          % (rep["bytes"], rep["lines"],
             "yes" if rep["final_eol"] else "no",
             "yes" if rep["trailing_blank"] else "no",
             rep["sha256"][:16]))
    print("  eol: CRLF only (crlf=%d, lone=0); encoding=%s"
          % (rep["crlf"], rep["encoding"]))
    st = rep["stats"]
    print("  lines=%d non-empty=%d blank=%d accepted=%d "
          "line-len=%d..%d accepted-len=%d..%d"
          % (st["lines"], st["non_empty"], st["blank"], st["accepted"],
             st["line_len_min"], st["line_len_max"],
             st["accepted_len_min"], st["accepted_len_max"]))
    print("  placeholders=%d (%%S=%d, %%s=%d), in accepted=%d, max per line=%d"
          % (st["ph_total"], st["ph_upper"], st["ph_lower"],
             st["ph_accepted"], st["ph_max_line"]))
    if rep.get("against"):
        print("  against %s (%d bytes): %s"
              % (rep["against"], rep["against_bytes"],
                 "IDENTICAL" if rep["identical"] else "differs"))
        if not rep["identical"]:
            print("    changed %d byte(s) (positional), first diff at %s, "
                  "delta %+d, lines %d -> %d, accepted %d -> %d, "
                  "placeholders in accepted %d -> %d"
                  % (rep["changed_bytes"],
                     "offset %s" % rep["first_diff"]
                     if rep["first_diff"] is not None else "n/a",
                     rep["delta"], rep["lines_before"], rep["lines"],
                     rep["accepted_before"], st["accepted"],
                     rep["ph_accepted_before"], st["ph_accepted"]))
            line = rep["first_changed_line"]
            if line:
                print("    first changed line %d: %r -> %r"
                      % (line["index"], line["before"], line["after"]))
    for warning in rep["warnings"]:
        print("  note: %s" % warning)
    for problem in rep["problems"]:
        print("  problem: %s" % problem)
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --list


def list_lines(path: str, accepted_only: bool = False) -> dict:
    doc = load(path)
    rows = line_rows(doc)
    shown = [r for r in rows if r["accepted"]] if accepted_only else rows
    st = doc_stats(doc["records"])
    return {"tool": "killedwrite", "mode": "list", "file": os.path.abspath(path),
            "bytes": doc["bytes"], "lines": doc["lines"],
            "final_eol": doc["final_eol"], "stats": st,
            "accepted_only": accepted_only, "rows": shown}


def print_list(rep) -> None:
    st = rep["stats"]
    print("list: %s (%d lines, %d non-empty, %d accepted by the filter, "
          "%d bytes%s)"
          % (rep["file"], rep["lines"], st["non_empty"], st["accepted"],
             rep["bytes"], ", accepted only" if rep["accepted_only"] else ""))
    print("  %4s %6s %4s %3s %2s  %s" % ("#", "off", "len", "ph", "ok", "text"))
    for row in rep["rows"]:
        text = esc(row["text"]) if row["text"] else "<blank>"
        flags = ",".join(f for f in row["flags"] if f != "accepted")
        print("  %4d %6d %4d %3d %2s  %s%s"
              % (row["index"], row["offset"], row["length"],
                 row["placeholders"], "+" if row["accepted"] else ".",
                 text, ("  " + flags) if flags else ""))


# ---------------------------------------------------------------------------
# --audit


def audit_target(path: str = "") -> tuple:
    """``(files, directory_or_None)`` for a file or a directory argument."""
    if not path:
        path = os.path.join(GAME, FRONTEND)
    if os.path.isfile(path):
        kind_of(path)
        return [path], None
    if not os.path.isdir(path):
        raise WriteError("%s is neither a file nor a directory" % path)
    files = killed_files(path)
    if not files:
        raise WriteError("no .skb/.skk files under %s" % path)
    return files, path


def audit_corpus(path: str = "") -> dict:
    files, directory = audit_target(path)
    started = time.time()
    rows = []
    totals = Counter()
    for file_path in files:
        rel = (os.path.relpath(file_path, directory).replace(os.sep, "/")
               if directory else os.path.basename(file_path))
        raw = read_bytes(file_path)
        doc = analyse(raw, rel)
        st = doc_stats(doc["records"])
        row = {"file": rel, "kind": os.path.splitext(file_path)[1].lower(),
               "bytes": len(raw), "crlf": raw.count(CRLF),
               "final_eol": doc["final_eol"],
               "trailing_blank": doc["trailing_blank"],
               "encoding": doc["encoding"]["ascii"] and "ascii" or "latin-1",
               "ascii": doc["encoding"]["ascii"],
               "high_bytes": doc["encoding"]["high_bytes"],
               "stats": st}
        for key in ("bytes", "lines", "non_empty", "blank", "accepted",
                    "rejected_long", "ph_total", "ph_accepted", "dups",
                    "dups_fold"):
            totals[key] += row["bytes"] if key == "bytes" else st[key]
        totals["crlf"] += row["crlf"]
        if doc["final_eol"]:
            totals["final_eol"] += 1
        if doc["trailing_blank"]:
            totals["trailing_blank"] += 1
        rows.append(row)
    rep = {
        "tool": "killedwrite", "mode": "audit",
        "dir": os.path.abspath(directory) if directory else None,
        "files": len(rows), "skb": sum(1 for r in rows if r["kind"] == ".skb"),
        "skk": sum(1 for r in rows if r["kind"] == ".skk"),
        "totals": dict(totals),
        "results": rows,
        "seconds": round(time.time() - started, 2),
    }
    rep["ok"] = (len(rows) == len(files)
                 and all(r["stats"]["accepted"] > 0 for r in rows))
    return rep


def print_audit(rep) -> None:
    print("audit: %s" % (rep["dir"] or "(single file)"))
    for row in rep["results"]:
        st = row["stats"]
        print("  %s" % row["file"])
        print("    bytes=%d crlf=%d lone=0 final-crlf=%s final-empty-line=%s "
              "encoding=%s"
              % (row["bytes"], row["crlf"],
                 "yes" if row["final_eol"] else "no",
                 "yes" if row["trailing_blank"] else "no", row["encoding"]))
        print("    lines=%d non-empty=%d blank=%d accepted=%d "
              "(longer than %d B: %d, stored %d/%d)"
              % (st["lines"], st["non_empty"], st["blank"], st["accepted"],
                 MAX_LINE_BYTES, st["rejected_long"], st["stored"],
                 MAX_MESSAGES))
        print("    line-len=%d..%d non-empty-len=%d..%d accepted-len=%d..%d"
              % (st["line_len_min"], st["line_len_max"],
                 st["nonempty_len_min"], st["nonempty_len_max"],
                 st["accepted_len_min"], st["accepted_len_max"]))
        print("    placeholders=%d (%%S=%d, %%s=%d), in accepted=%d, "
              "max per line=%d"
              % (st["ph_total"], st["ph_upper"], st["ph_lower"],
                 st["ph_accepted"], st["ph_max_line"]))
        print("    duplicates=%d extra occurrence(s) in %d line(s) "
              "(fold: %d in %d)"
              % (st["dups"], st["dup_lines"], st["dups_fold"],
                 st["dup_lines_fold"]))
    tot = rep["totals"]
    print("  totals: files=%d (%d .skb + %d .skk) bytes=%d lines=%d "
          "non-empty=%d accepted=%d placeholders=%d (in accepted %d) dups=%d"
          % (rep["files"], rep["skb"], rep["skk"], tot.get("bytes", 0),
             tot.get("lines", 0), tot.get("non_empty", 0),
             tot.get("accepted", 0), tot.get("ph_total", 0),
             tot.get("ph_accepted", 0), tot.get("dups", 0)))
    print("  %.2fs" % rep["seconds"])
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --roundtrip


def roundtrip_corpus(path: str = "", verbose: bool = False) -> dict:
    files, directory = audit_target(path)
    started = time.time()
    rows = []
    counts = Counter()
    totals = Counter()
    for file_path in files:
        rel = (os.path.relpath(file_path, directory).replace(os.sep, "/")
               if directory else os.path.basename(file_path))
        raw = read_bytes(file_path)
        row = {"file": rel, "bytes": len(raw), "problems": []}
        totals["bytes"] += len(raw)
        try:
            doc = analyse(raw, rel)
        except WriteError as exc:
            counts["error"] += 1
            row["status"] = "error"
            row["reason"] = str(exc)
            rows.append(row)
            continue
        out = to_bytes(doc["records"], doc["final_eol"])
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
        oracle = smallmod.parse_kill(raw)
        if oracle.to_bytes() == raw:
            counts["oracle_bytes"] += 1
        else:
            row["problems"].append("gm/small.py to_bytes != file")
        if oracle.lines == doc["records"]:
            counts["oracle_lines"] += 1
        else:
            row["problems"].append("gm/small.py lines differ")
        st = doc_stats(doc["records"])
        row.update({"lines": st["lines"], "non_empty": st["non_empty"],
                    "accepted": st["accepted"], "placeholders": st["ph_total"],
                    "final_eol": doc["final_eol"],
                    "trailing_blank": doc["trailing_blank"]})
        totals["lines"] += st["lines"]
        totals["non_empty"] += st["non_empty"]
        totals["accepted"] += st["accepted"]
        rows.append(row)
        if verbose:
            print("  %-20s %-9s %5d B  %3d lines, %3d accepted, %3d placeholders"
                  % (rel, row["status"], len(raw), st["lines"],
                     st["accepted"], st["ph_total"]))
    rep = {
        "tool": "killedwrite", "mode": "roundtrip",
        "dir": os.path.abspath(directory) if directory else None,
        "file": None if directory else os.path.abspath(files[0]),
        "files": len(files),
        "skb": sum(1 for f in files if f.lower().endswith(".skb")),
        "skk": sum(1 for f in files if f.lower().endswith(".skk")),
        "identical": counts["identical"], "differs": counts["differs"],
        "errors": counts["error"], "bytes": totals["bytes"],
        "lines": totals["lines"], "non_empty": totals["non_empty"],
        "accepted": totals["accepted"],
        "oracle_identical": counts["oracle_bytes"],
        "oracle_lines_match": counts["oracle_lines"],
        "seconds": round(time.time() - started, 2),
        "results": rows,
    }
    rep["ok"] = (counts["identical"] == len(files)
                 and not counts["differs"] and not counts["error"]
                 and counts["oracle_bytes"] == len(files)
                 and counts["oracle_lines"] == len(files))
    return rep


def print_roundtrip(rep) -> None:
    print("roundtrip: %s" % (rep["dir"] or rep["file"]))
    print("  %d files (%d .skb + %d .skk), %d bytes, %d lines (%d non-empty, "
          "%d accepted), %.2fs"
          % (rep["files"], rep["skb"], rep["skk"], rep["bytes"], rep["lines"],
             rep["non_empty"], rep["accepted"], rep["seconds"]))
    print("  identical=%d/%d  differs=%d  errors=%d"
          % (rep["identical"], rep["files"], rep["differs"], rep["errors"]))
    print("  oracle gm/small.py: to_bytes identical=%d/%d, lines match=%d/%d"
          % (rep["oracle_identical"], rep["files"],
             rep["oracle_lines_match"], rep["files"]))
    for row in rep["results"]:
        if row["status"] != "identical":
            print("  %-20s %-9s %s"
                  % (row["file"], row["status"], row.get("reason", "")))
        for problem in row["problems"]:
            if problem != row.get("reason"):
                print("  problem: %s: %s" % (row["file"], problem))
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# editing


def validate_text(text: str) -> dict:
    """Check a new message; returns warnings, raises on an impossible one."""
    if "\r" in text or "\n" in text:
        raise WriteError("--text must not contain CR/LF: a message is one line")
    try:
        data = text.encode("latin1")
    except UnicodeEncodeError:
        raise WriteError("--text %r is not latin1-encodable; the kill tables "
                         "are one-byte text" % text)
    if b"\x00" in data:
        raise WriteError("--text must not contain a NUL byte (strlen would cut "
                         "the message there)")
    high = sorted({b for b in data if b > 0x7F})
    if high:
        raise WriteError(
            "--text must be ASCII: byte(s) %s are not (both shipped files are "
            "pure ASCII; latin-1 is only readable, not writable here)"
            % ", ".join("0x%02x" % b for b in high[:8]))
    control = sorted({b for b in data if b < 0x20 or b == 0x7F})
    if control:
        raise WriteError(
            "--text must not contain control byte(s) %s: use spaces "
            "(a message is printable text)"
            % ", ".join("0x%02x" % b for b in control[:8]))
    upper, lower = count_placeholders(text)
    if upper + lower > MAX_PLACEHOLDERS:
        raise WriteError(
            "--text has %d placeholder(s) (%%S/%%s), at most %d fit: the "
            "templates name the killer and the victim (%r)"
            % (upper + lower, MAX_PLACEHOLDERS, "%S KILLS %S."))
    if len(data) > MAX_TEXT_BYTES:
        raise WriteError(
            "--text is %d bytes; fgets(buf, 0x400) reads at most 1023 bytes "
            "including the terminator, so a longer line would arrive as extra "
            "messages (limit %d)" % (len(data), MAX_TEXT_BYTES))
    warnings = []
    if not data:
        warnings.append("empty text: the record becomes an empty line and is "
                        "filtered out (accepted count changes)")
    elif not text.strip(SPACES):
        warnings.append("whitespace-only text: the line prints nothing and the "
                        "engine's length filter still keeps it, the reader "
                        "skips it (docs/KILLED.md §2.5)")
    if len(data) > MAX_LINE_BYTES:
        warnings.append(
            "the line is %d bytes: the engine keeps only 1..%d visible bytes, "
            "so this message is dropped (accepted count changes)"
            % (len(data), MAX_LINE_BYTES))
    return {"bytes": data, "warnings": warnings,
            "placeholders": upper + lower}


def refuse_non_ascii(doc: dict, path: str) -> None:
    high = doc["encoding"]["high_bytes"]
    if high:
        raise WriteError(
            "refusing to edit %s: it has non-ASCII byte(s) %s. The writer "
            "rewrites ASCII only (both shipped files are pure ASCII); --check "
            "still reads such a file"
            % (path, ", ".join("0x%02x" % b for b in high[:8])))


def _accept_change(before: dict, after: dict) -> list:
    warnings = []
    if before["accepted"] != after["accepted"]:
        warnings.append(
            "the accepted set changes: %d -> %d line(s) (the engine keeps "
            "1..%d visible bytes); placeholders in accepted: %d -> %d"
            % (before["accepted"], after["accepted"], MAX_LINE_BYTES,
               before["ph_accepted"], after["ph_accepted"]))
    if after["over_limit"] and after["accepted"] > before["accepted"]:
        warnings.append(
            "%d line(s) pass the filter but the engine stores at most %d "
            "messages: the tail is ignored" % (after["accepted"], MAX_MESSAGES))
    return warnings


def _prove(new_raw: bytes, records, doc: dict, out: str) -> dict:
    """Re-read the output, cross-check the oracle and run ``--check``."""
    redo = analyse(new_raw, out)
    if redo["records"] != records:
        raise WriteError("internal: re-reading the output does not give the "
                         "expected records")
    if redo["raw"] != new_raw:
        raise WriteError("internal: the output does not re-serialise to itself")
    if to_bytes(records, doc["final_eol"]) != new_raw:
        raise WriteError("internal: the output is not the serialisation of the "
                         "records")
    oracle = smallmod.parse_kill(new_raw)
    if oracle.lines != records:
        raise WriteError("internal: gm/small.py splits the output differently")
    if oracle.to_bytes() != new_raw:
        raise WriteError("internal: gm/small.py does not re-serialise the output")
    verdict = doc_problems(redo, out)
    if verdict["problems"]:
        raise WriteError("internal: the edited file does not pass --check: %s"
                         % "; ".join(verdict["problems"]))
    return {"redo": redo, "verdict": verdict}


def _publish(out: str, new_raw: bytes) -> str:
    write_bytes(out, new_raw)
    digest = sha256(read_bytes(out))
    if digest != sha256(new_raw):
        raise WriteError("internal: %s on disk does not match the report" % out)
    return digest


def set_line(path: str, line: int, text: str, out: str) -> dict:
    """Replace the text of record *line*; every other byte stays."""
    out = guard_out(out, path)
    doc = load(path)
    refuse_non_ascii(doc, path)
    records = doc["records"]
    count = len(records)
    if line < 1 or line > count:
        raise WriteError("--set %d is out of range: %s has %d record(s) (1..%d)"
                         % (line, path, count, count))
    valid = validate_text(text)
    old = records[line - 1]
    new_records = list(records)
    new_records[line - 1] = text
    new_raw = to_bytes(new_records, doc["final_eol"])

    # proof 1: one record changed, the count and every neighbour are untouched
    if len(new_records) != count:
        raise WriteError("internal: the edit changed the record count")
    if new_records[:line - 1] != records[:line - 1] or \
            new_records[line:] != records[line:]:
        raise WriteError("internal: the edit changed a neighbouring record")
    if new_raw.count(CRLF) != doc["raw"].count(CRLF):
        raise WriteError("internal: the edit changed the CRLF count")
    if new_raw.endswith(CRLF) != doc["final_eol"]:
        raise WriteError("internal: the edit changed the final-EOL state")

    proofs = _prove(new_raw, new_records, doc, out)
    before = doc_stats(records)
    after = doc_stats(new_records)
    warnings = list(valid["warnings"]) + _accept_change(before, after)
    line_stat = diff_stats(old.encode("latin1"), text.encode("latin1"))
    file_stat = diff_stats(doc["raw"], new_raw)
    rep = {
        "tool": "killedwrite", "mode": "set", "file": path, "out": out,
        "line": line, "lines": count,
        "old_text": old, "new_text": text,
        "old_length": len(old.encode("latin1")), "new_length": len(valid["bytes"]),
        "placeholders": valid["placeholders"],
        "bytes_in": len(doc["raw"]), "bytes_out": len(new_raw),
        "delta": len(new_raw) - len(doc["raw"]),
        "changed_bytes": line_stat["changed"],
        "first_diff": line_stat["first_diff"],
        "file_changed_bytes": file_stat["changed"],
        "file_first_diff": file_stat["first_diff"],
        "accepted_before": before["accepted"], "accepted_after": after["accepted"],
        "ph_accepted_before": before["ph_accepted"],
        "ph_accepted_after": after["ph_accepted"],
        "sha256_in": sha256(doc["raw"]), "sha256_out": sha256(new_raw),
        "final_eol": doc["final_eol"], "trailing_blank": doc["trailing_blank"],
        "layout_same": True, "warnings": warnings,
        "check_ok": not proofs["verdict"]["problems"],
    }
    rep["sha256_out"] = _publish(out, new_raw)
    return rep


def print_set(rep) -> None:
    print("edit: %s -> %s (line %d/%d)"
          % (rep["file"], rep["out"], rep["line"], rep["lines"]))
    print("  text: %r -> %r" % (rep["old_text"], rep["new_text"]))
    print("  length: %d -> %d B, %d placeholder(s); changed %d byte(s) in the "
          "line (first at offset %s), file %+d"
          % (rep["old_length"], rep["new_length"], rep["placeholders"],
             rep["changed_bytes"],
             rep["first_diff"] if rep["first_diff"] is not None else "n/a",
             rep["delta"]))
    print("  accepted: %d -> %d line(s); placeholders in accepted: %d -> %d"
          % (rep["accepted_before"], rep["accepted_after"],
             rep["ph_accepted_before"], rep["ph_accepted_after"]))
    print("  bytes: %d -> %d, CRLF/final empty line preserved; sha256: %s -> %s"
          % (rep["bytes_in"], rep["bytes_out"], rep["sha256_in"][:16],
             rep["sha256_out"][:16]))
    print("  gm/small.py re-reads the output; --check: %s"
          % ("ok" if rep["check_ok"] else "FAILED"))
    for warning in rep["warnings"]:
        print("  warning: %s" % warning)


def add_line(path: str, text: str, out: str, line: int | None = None,
             mode: str = "append") -> dict:
    """Insert one record, keeping the trailing empty CRLF line last."""
    out = guard_out(out, path)
    doc = load(path)
    refuse_non_ascii(doc, path)
    valid = validate_text(text)
    if not valid["bytes"]:
        raise WriteError("--text must not be empty: adding an empty line adds "
                         "nothing (use --set to blank an existing line)")
    records = list(doc["records"])
    count = len(records)
    if mode == "append":
        if not doc["final_eol"]:
            raise WriteError(
                "refusing to append to %s: it does not end with CRLF, so an "
                "appended line would change the tail structure (both shipped "
                "files end with an empty CRLF line); use --set" % path)
        position = count
        before_blank = bool(records) and records[-1] == ""
        if before_blank:
            position = count - 1
        where = ("before the trailing empty line (line %d of %d)"
                 % (count, count + 1) if before_blank
                 else "after the last line (line %d of %d)"
                 % (count + 1, count + 1))
    else:
        if line is None:
            raise WriteError("--prepend needs LINE")
        if line < 1 or line > count:
            raise WriteError(
                "--prepend %d is out of range: %s has %d record(s) (1..%d); "
                "use --append for the end" % (line, path, count, count))
        position = line - 1
        where = "before line %d of %d" % (line, count + 1)
    new_records = records[:position] + [text] + records[position:]
    new_raw = to_bytes(new_records, doc["final_eol"])

    # proof 1: exactly one record added, order and tail untouched
    if len(new_records) != count + 1:
        raise WriteError("internal: the edit did not add exactly one record")
    if new_records[:position] != records[:position] or \
            new_records[position + 1:] != records[position:]:
        raise WriteError("internal: the edit changed an existing record")
    if new_raw.count(CRLF) != doc["raw"].count(CRLF) + 1:
        raise WriteError("internal: the edit did not add exactly one CRLF")
    if new_raw.endswith(CRLF) != doc["final_eol"]:
        raise WriteError("internal: the edit changed the final-EOL state")
    if doc["trailing_blank"] and new_records[-1] != "":
        raise WriteError("internal: the edit moved the trailing empty line")

    proofs = _prove(new_raw, new_records, doc, out)
    before = doc_stats(records)
    after = doc_stats(new_records)
    warnings = list(valid["warnings"]) + _accept_change(before, after)
    if doc["trailing_blank"]:
        warnings.append("the empty CRLF line at the end stays last "
                        "(the file still ends with CRLF CRLF)")
    file_stat = diff_stats(doc["raw"], new_raw)
    inserted = len(valid["bytes"]) + len(CRLF)
    rep = {
        "tool": "killedwrite", "mode": mode, "file": path, "out": out,
        "line": None if mode == "append" else line,
        "position": position + 1, "where": where,
        "text": text, "length": len(valid["bytes"]),
        "placeholders": valid["placeholders"],
        "lines_before": count, "lines": count + 1,
        "bytes_in": len(doc["raw"]), "bytes_out": len(new_raw),
        "delta": len(new_raw) - len(doc["raw"]),
        "inserted_bytes": inserted,
        "file_changed_bytes": file_stat["changed"],
        "file_first_diff": file_stat["first_diff"],
        "accepted_before": before["accepted"], "accepted_after": after["accepted"],
        "ph_accepted_before": before["ph_accepted"],
        "ph_accepted_after": after["ph_accepted"],
        "sha256_in": sha256(doc["raw"]), "sha256_out": sha256(new_raw),
        "final_eol": doc["final_eol"], "trailing_blank": doc["trailing_blank"],
        "layout_same": True, "warnings": warnings,
        "check_ok": not proofs["verdict"]["problems"],
    }
    rep["sha256_out"] = _publish(out, new_raw)
    return rep


def print_add(rep) -> None:
    print("%s: %s -> %s (%s)" % (rep["mode"], rep["file"], rep["out"],
                                 rep["where"]))
    print("  text: %r (%d B, %d placeholder(s))"
          % (rep["text"], rep["length"], rep["placeholders"]))
    print("  lines: %d -> %d, bytes: %d -> %d (%+d), inserted %d byte(s) "
          "(text + CRLF)"
          % (rep["lines_before"], rep["lines"], rep["bytes_in"],
             rep["bytes_out"], rep["delta"], rep["inserted_bytes"]))
    print("  accepted: %d -> %d line(s); placeholders in accepted: %d -> %d"
          % (rep["accepted_before"], rep["accepted_after"],
             rep["ph_accepted_before"], rep["ph_accepted_after"]))
    print("  file diff (positional, an insertion shifts the tail): %d byte(s), "
          "first at offset %s"
          % (rep["file_changed_bytes"], rep["file_first_diff"]))
    print("  sha256: %s -> %s; gm/small.py re-reads the output; --check: %s"
          % (rep["sha256_in"][:16], rep["sha256_out"][:16],
             "ok" if rep["check_ok"] else "FAILED"))
    for warning in rep["warnings"]:
        print("  warning: %s" % warning)


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
    return tempfile.mkdtemp(prefix="killedwrite-selftest-", dir=base)


def _selftest(json_out: bool = False) -> bool:
    results = []
    work = _selftest_workdir()
    good = b"%S KILLS %S.\r\n%s IS DEAD.\r\n\r\n"

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
            return "refused: %s" % str(exc)[:58]
        raise AssertionError("expected WriteError containing %r" % needle)

    def path(name, data=None):
        p = os.path.join(work, name)
        if data is not None:
            os.makedirs(os.path.dirname(p) or work, exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        return p

    # --- model ------------------------------------------------------------
    def t_split_join():
        doc = analyse(good)
        assert doc["records"] == ["%S KILLS %S.", "%s IS DEAD.", ""]
        assert to_bytes(doc["records"], doc["final_eol"]) == good
        return "%d bytes, %d records" % (len(good), doc["lines"])

    def t_crlf_crlf():
        raw = b"%S A %S.\r\n%S B %S.\r\n\r\n"
        doc = analyse(raw)
        assert doc["trailing_blank"] and doc["final_eol"]
        assert raw.endswith(b"\r\n\r\n")
        assert to_bytes(doc["records"], True) == raw
        return "trailing empty line kept (CRLF CRLF)"

    def t_no_final_eol():
        raw = b"%S A %S.\r\n%S B %S."
        doc = analyse(raw)
        assert not doc["final_eol"] and not doc["trailing_blank"]
        assert to_bytes(doc["records"], False) == raw
        return "last line without CRLF preserved"

    def t_lone_lf():
        return expect_error(lambda: analyse(b"A\r\nB\n"), "lone CR/LF")

    def t_lone_cr():
        return expect_error(lambda: analyse(b"A\rB\r\n"), "lone CR/LF")

    def t_empty():
        return expect_error(lambda: analyse(b""), "empty file")

    def t_nul_refused():
        return expect_error(lambda: analyse(b"%S A.\x00B\r\n"), "NUL")

    def t_too_big():
        return expect_error(lambda: analyse(b"A" * (MAX_FILE_BYTES + 1)),
                            "larger than")

    def t_extension():
        return expect_error(lambda: kind_of(path("x.txt", good)), "not a .skb")

    def t_kinds_ok():
        assert kind_of("A.SKB") == ".skb" and kind_of("A.skk") == ".skk"
        return ".skb/.skk accepted in both cases"

    def t_latin1_roundtrip():
        raw = "caf\u00e9 %S.\r\n".encode("latin1")
        doc = analyse(raw)
        assert to_bytes(doc["records"], True) == raw
        assert not doc["encoding"]["ascii"]
        return "high byte read and re-written 1:1"

    def t_placeholders():
        assert count_placeholders("%S KILLS %s.") == (1, 1)
        assert count_placeholders("50%% OFF, %S.") == (1, 0)
        assert count_placeholders("%Sx%S") == (2, 0)
        return "counts match ui/killed.cpp"

    # --- check ------------------------------------------------------------
    def t_check_ok():
        rep = check_file(path("ok.skb", good))
        assert rep["ok"] and not rep["problems"], rep["problems"]
        st = rep["stats"]
        assert (st["lines"], st["accepted"], st["ph_total"],
                st["ph_accepted"]) == (3, 2, 3, 3), st
        assert rep["ascii"] and rep["final_eol"] and rep["trailing_blank"]
        return "3 lines, 2 accepted, 3 placeholders"

    def t_check_no_message():
        rep = check_file(path("long.skk", b"%S " + b"A" * 40 + b"\r\n"))
        assert not rep["ok"] and "no message" in rep["problems"][0]
        return "a file whose every line is dropped is a problem"

    def t_check_long_warns():
        raw = b"%S KILLS %S.\r\n%S " + b"A" * 30 + b"\r\n\r\n"
        rep = check_file(path("long2.skb", raw))
        assert rep["ok"] and any("longer than 30" in w for w in rep["warnings"])
        assert rep["stats"]["rejected_long"] == 1
        return "31-byte line: warning, not a problem"

    def t_check_against_same():
        a = path("same.skb", good)
        rep = check_file(a, a)
        assert rep["identical"] and rep["changed_bytes"] == 0
        return "identical against itself"

    def t_check_against_diff():
        a = path("orig.skb", good)
        b = path("edit.skb", b"%S KILLS %s.\r\n%s IS DEAD.\r\n\r\n")
        rep = check_file(b, a)
        assert rep["structure_ok"] and not rep["identical"]
        assert rep["changed_bytes"] == 1, rep["changed_bytes"]
        assert rep["first_changed_line"]["index"] == 1
        return "one changed byte, line 1"

    def t_check_non_ascii():
        rep = check_file(path("l1.skb", "caf\u00e9\r\n".encode("latin1")))
        assert rep["ok"] and rep["encoding"] == "latin-1" and rep["high_bytes"]
        assert any("non-ASCII" in w for w in rep["warnings"])
        return "latin-1 readable, flagged"

    def t_check_no_final_eol_warns():
        rep = check_file(path("noeol.skb", b"%S KILLS %S."))
        assert rep["ok"] and any("does not end with CRLF" in w
                                for w in rep["warnings"])
        return "unterminated tail: note"

    def t_check_no_trailing_blank():
        rep = check_file(path("noblank.skb", b"%S KILLS %S.\r\n"))
        assert rep["ok"] and any("not empty" in w for w in rep["warnings"])
        return "single CRLF tail: note"

    # --- list -------------------------------------------------------------
    def t_list_rows():
        rep = list_lines(path("list.skb", good))
        assert [r["index"] for r in rep["rows"]] == [1, 2, 3]
        assert rep["rows"][0]["accepted"] and not rep["rows"][2]["accepted"]
        assert rep["rows"][0]["placeholders"] == 2
        assert "blank" in rep["rows"][2]["flags"]
        return "%d rows" % len(rep["rows"])

    def t_list_accepted():
        rep = list_lines(path("list.skb", good), accepted_only=True)
        assert [r["index"] for r in rep["rows"]] == [1, 2]
        return "2 accepted rows"

    # --- set --------------------------------------------------------------
    def t_set_one_byte():
        src = path("set-in.skb", good)
        dst = path("set-out.skb")
        rep = set_line(src, 1, "%S KILLS %s.", dst)
        assert read_bytes(dst) == b"%S KILLS %s.\r\n%s IS DEAD.\r\n\r\n"
        assert rep["changed_bytes"] == 1 and rep["delta"] == 0
        assert rep["accepted_before"] == rep["accepted_after"] == 2
        return "changed 1 byte, layout same"

    def t_set_length_change():
        src = path("len-in.skb", good)
        dst = path("len-out.skb")
        rep = set_line(src, 2, "%S IS NO MORE.", dst)
        assert read_bytes(dst) == b"%S KILLS %S.\r\n%S IS NO MORE.\r\n\r\n"
        assert rep["delta"] == 3 and rep["changed_bytes"] >= 1
        return "delta +1, CRLF count unchanged"

    def t_set_accepted_change():
        src = path("acc-in.skb", b"%S A %S.\r\n%S " + b"B" * 30 + b"\r\n\r\n")
        dst = path("acc-out.skb")
        rep = set_line(src, 2, "%S " + "B" * 27, dst)
        assert rep["accepted_before"] == 1 and rep["accepted_after"] == 2
        assert any("accepted set changes" in w for w in rep["warnings"])
        return "accepted 1 -> 2, warned"

    def t_set_empty_text():
        src = path("empty-in.skb", good)
        dst = path("empty-out.skb")
        rep = set_line(src, 2, "", dst)
        assert read_bytes(dst) == b"%S KILLS %S.\r\n\r\n\r\n"
        assert rep["accepted_before"] == 2 and rep["accepted_after"] == 1
        assert any("empty text" in w for w in rep["warnings"])
        return "blanked line, warned"

    def t_set_range():
        src = path("range.skb", good)
        return expect_error(lambda: set_line(src, 9, "X", path("r1.skb")),
                            "out of range")

    def t_set_zero():
        src = path("zero.skb", good)
        return expect_error(lambda: set_line(src, 0, "X", path("z1.skb")),
                            "out of range")

    def t_set_crlf_text():
        src = path("crlf.skb", good)
        return expect_error(lambda: set_line(src, 1, "A\r\nB", path("c1.skb")),
                            "CR/LF")

    def t_set_nul_text():
        src = path("nul.skb", good)
        return expect_error(lambda: set_line(src, 1, "A\x00B", path("n1.skb")),
                            "NUL")

    def t_set_non_ascii_text():
        src = path("hi.skb", good)
        return expect_error(
            lambda: set_line(src, 1, "caf\u00e9 %S", path("h1.skb")), "ASCII")

    def t_set_control_text():
        src = path("ctl.skb", good)
        return expect_error(
            lambda: set_line(src, 1, "A\tB", path("t1.skb")), "control")

    def t_set_three_placeholders():
        src = path("ph.skb", good)
        return expect_error(
            lambda: set_line(src, 1, "%S %S %S", path("p1.skb")),
            "placeholder")

    def t_set_two_placeholders():
        src = path("ph2.skb", good)
        rep = set_line(src, 1, "%S KILLS %S.", path("p2.skb"))
        assert rep["placeholders"] == 2
        return "two placeholders accepted"

    def t_set_long_text():
        src = path("big.skb", good)
        return expect_error(
            lambda: set_line(src, 1, "A" * (MAX_TEXT_BYTES + 1), path("b1.skb")),
            "1022")

    def t_set_non_ascii_file():
        src = path("hi-in.skb", "caf\u00e9\r\n".encode("latin1"))
        return expect_error(
            lambda: set_line(src, 1, "A", path("hi-out.skb")),
            "refusing to edit")

    def t_set_out_of_game():
        src = path("g-in.skb", good)
        return expect_error(
            lambda: set_line(src, 1, "A", os.path.join(DEFAULT_GAME, "x.skb")),
            "refusing to write")

    # --- prepend / append -------------------------------------------------
    def t_append_keeps_tail():
        src = path("app-in.skb", good)
        dst = path("app-out.skb")
        rep = add_line(src, "%S NEW %S.", dst, mode="append")
        assert read_bytes(dst) == \
            b"%S KILLS %S.\r\n%s IS DEAD.\r\n%S NEW %S.\r\n\r\n"
        assert rep["lines_before"] == 3 and rep["lines"] == 4
        assert rep["trailing_blank"] and rep["position"] == 3
        return "inserted before the trailing empty line"

    def t_append_no_trailing_blank():
        src = path("app2-in.skb", b"%S KILLS %S.\r\n")
        dst = path("app2-out.skb")
        rep = add_line(src, "%S NEW %S.", dst, mode="append")
        assert read_bytes(dst) == b"%S KILLS %S.\r\n%S NEW %S.\r\n"
        assert rep["position"] == 2
        return "no trailing empty line: appended last"

    def t_append_no_final_eol():
        src = path("app3-in.skb", b"%S KILLS %S.")
        return expect_error(
            lambda: add_line(src, "X", path("app3-out.skb"), mode="append"),
            "does not end with CRLF")

    def t_prepend_first():
        src = path("pre-in.skb", good)
        dst = path("pre-out.skb")
        rep = add_line(src, "%S FIRST.", dst, line=1, mode="prepend")
        assert read_bytes(dst) == \
            b"%S FIRST.\r\n%S KILLS %S.\r\n%s IS DEAD.\r\n\r\n"
        assert rep["position"] == 1 and rep["trailing_blank"]
        return "prepended before line 1"

    def t_prepend_range():
        src = path("pre2-in.skb", good)
        return expect_error(
            lambda: add_line(src, "X", path("pre2-out.skb"), line=4,
                             mode="prepend"), "out of range")

    def t_prepend_zero():
        src = path("pre3-in.skb", good)
        return expect_error(
            lambda: add_line(src, "X", path("pre3-out.skb"), line=0,
                             mode="prepend"), "out of range")

    def t_append_long_warns():
        src = path("app4-in.skb", good)
        rep = add_line(src, "%S " + "A" * 40, path("app4-out.skb"),
                       mode="append")
        assert any("dropped" in w for w in rep["warnings"])
        assert rep["accepted_after"] == 2
        return "41-byte line added but filtered out"

    def t_append_empty_text():
        src = path("app5-in.skb", good)
        return expect_error(
            lambda: add_line(src, "", path("app5-out.skb"), mode="append"),
            "must not be empty")

    # --- guards / corpus --------------------------------------------------
    def t_guard_game():
        return expect_error(
            lambda: guard_out(os.path.join(DEFAULT_GAME, FRONTEND, "x.skb")),
            "refusing to write")

    def t_guard_mirror():
        return expect_error(
            lambda: guard_out(os.path.join(MIRROR_GAME, FRONTEND, "x.skk")),
            "refusing to write")

    def t_guard_repo():
        return expect_error(
            lambda: guard_out(os.path.join(ROOT, "x.skb")), "refusing to write")

    def t_guard_main_repo():
        return expect_error(
            lambda: guard_out(os.path.join(MAIN_REPO, "x.skb")),
            "refusing to write")

    def t_guard_same_file():
        src = path("same-in.skb", good)
        return expect_error(lambda: guard_out(src, src), "refusing to overwrite")

    def t_audit_synth():
        d = os.path.join(work, "corpus")
        os.makedirs(os.path.join(d, "sub"), exist_ok=True)
        with open(os.path.join(d, "a.skb"), "wb") as f:
            f.write(b"%S KILLS %S.\r\n%S IS DEAD.\r\n%S " + b"A" * 25 + b"\r\n\r\n")
        with open(os.path.join(d, "sub", "b.skk"), "wb") as f:
            f.write(b"%S BITES.\r\n\r\n")
        rep = audit_corpus(d)
        assert rep["files"] == 2 and rep["skb"] == 1 and rep["skk"] == 1, rep
        tot = rep["totals"]
        assert (tot["bytes"], tot["lines"], tot["accepted"],
                tot["ph_total"]) == (72, 6, 4, 5), tot
        return "files=2 lines=6 accepted=4 placeholders=5"

    def t_roundtrip_synth():
        d = os.path.join(work, "rt")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "a.skb"), "wb") as f:
            f.write(b"%S KILLS %S.\r\n\r\n")
        with open(os.path.join(d, "b.skk"), "wb") as f:
            f.write(b"%S IS DEAD.\r\n")
        rep = roundtrip_corpus(d)
        assert rep["ok"], rep["results"]
        assert rep["identical"] == 2 and rep["oracle_identical"] == 2
        assert rep["oracle_lines_match"] == 2
        return "identical=2/2, oracle=2/2"

    def t_roundtrip_lf_refused():
        d = os.path.join(work, "rt-lf")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "a.skb"), "wb") as f:
            f.write(b"%S KILLS %S.\n\n")
        rep = roundtrip_corpus(d)
        assert not rep["ok"] and rep["errors"] == 1, rep
        assert "lone CR/LF" in rep["results"][0]["reason"]
        return "LF file reported, not normalised"

    def t_roundtrip_single_file():
        f = path("one.skk", b"%S BITES.\r\n\r\n")
        rep = roundtrip_corpus(f)
        assert rep["files"] == 1 and rep["ok"] and rep["file"]
        return "single file: identical=1/1"

    def t_helpers():
        assert diff_stats(b"abc", b"axc") == {"changed": 1, "first_diff": 1,
                                              "delta": 0}
        assert diff_stats(b"abc", b"ab") == {"changed": 1, "first_diff": 2,
                                             "delta": -1}
        assert sha256(b"") == \
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        assert esc("A\x00\xff") == "A\\x00\\xff" and esc("A\tB") == "A\\tB"
        return "diff/sha/escaping"

    cases = [
        ("model: split/join identity", t_split_join),
        ("model: CRLF CRLF tail", t_crlf_crlf),
        ("model: last line without CRLF", t_no_final_eol),
        ("model: lone LF refused", t_lone_lf),
        ("model: lone CR refused", t_lone_cr),
        ("model: empty file refused", t_empty),
        ("model: NUL refused", t_nul_refused),
        ("model: >64 KiB refused", t_too_big),
        ("model: extension refused", t_extension),
        ("model: .skb/.skk accepted", t_kinds_ok),
        ("model: latin-1 is 1 byte/char", t_latin1_roundtrip),
        ("model: placeholder counting", t_placeholders),
        ("check: structure ok", t_check_ok),
        ("check: no accepted line", t_check_no_message),
        ("check: long line warns", t_check_long_warns),
        ("check: --against identical", t_check_against_same),
        ("check: --against diff", t_check_against_diff),
        ("check: non-ASCII reported", t_check_non_ascii),
        ("check: no final CRLF note", t_check_no_final_eol_warns),
        ("check: no trailing blank note", t_check_no_trailing_blank),
        ("list: rows/flags", t_list_rows),
        ("list: --accepted", t_list_accepted),
        ("set: one byte", t_set_one_byte),
        ("set: length change", t_set_length_change),
        ("set: accepted set changes", t_set_accepted_change),
        ("set: empty text", t_set_empty_text),
        ("set: line out of range", t_set_range),
        ("set: line 0", t_set_zero),
        ("set: CR/LF text", t_set_crlf_text),
        ("set: NUL text", t_set_nul_text),
        ("set: non-ASCII text", t_set_non_ascii_text),
        ("set: control text", t_set_control_text),
        ("set: three placeholders", t_set_three_placeholders),
        ("set: two placeholders", t_set_two_placeholders),
        ("set: text >1022 B", t_set_long_text),
        ("set: non-ASCII file refused", t_set_non_ascii_file),
        ("set: --out in the game", t_set_out_of_game),
        ("append: keeps trailing blank", t_append_keeps_tail),
        ("append: no trailing blank", t_append_no_trailing_blank),
        ("append: no final CRLF refused", t_append_no_final_eol),
        ("prepend: before line 1", t_prepend_first),
        ("prepend: out of range", t_prepend_range),
        ("prepend: line 0", t_prepend_zero),
        ("append: long line warned", t_append_long_warns),
        ("append: empty text refused", t_append_empty_text),
        ("guard: game", t_guard_game),
        ("guard: mirror", t_guard_mirror),
        ("guard: worktree", t_guard_repo),
        ("guard: main repository", t_guard_main_repo),
        ("guard: same file", t_guard_same_file),
        ("audit: synthetic corpus", t_audit_synth),
        ("roundtrip: synthetic corpus", t_roundtrip_synth),
        ("roundtrip: LF file reported", t_roundtrip_lf_refused),
        ("roundtrip: single file", t_roundtrip_single_file),
        ("helpers: diff/sha/escape", t_helpers),
    ]
    for name, fn in cases:
        case(name, fn)

    shutil.rmtree(work, ignore_errors=True)
    passed = sum(1 for _n, ok, _d in results if ok)
    if json_out:
        print(json.dumps({
            "tool": "killedwrite", "mode": "self-test",
            "passed": passed, "total": len(results),
            "ok": passed == len(results),
            "cases": [{"name": n, "ok": ok, "detail": d}
                      for n, ok, d in results],
        }, indent=1))
    else:
        for name, ok, detail in results:
            print("  %-34s %s  %s" % (name, "ok  " if ok else "FAIL", detail))
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
    rep = list_lines(args.list, args.accepted)
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
    if args.text is None:
        raise WriteError("--set needs --text TEXT")
    if not args.out:
        raise WriteError("--set needs --out OUT (never the input file)")
    rep = set_line(args.set, args.line, args.text, args.out)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_set(rep)
    return 0 if rep["check_ok"] else 1


def _cmd_add(args) -> int:
    if args.text is None:
        raise WriteError("--%s needs --text TEXT" % args.mode)
    if not args.out:
        raise WriteError("--%s needs --out OUT (never the input file)"
                         % args.mode)
    rep = add_line(args.mode_file, args.text, args.out,
                   line=args.line, mode=args.mode)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_add(rep)
    return 0 if rep["check_ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(
        description="Writer/editor for the Gunmetal kill-message tables "
                    "FRONTEND/KILLED.skb and KILLED.skk (M11.5).")
    ap.add_argument("--roundtrip", nargs="?", const="", metavar="DIR|FILE",
                    help="re-serialise every .skb/.skk and compare byte for "
                         "byte (default game/FRONTEND, recursive)")
    ap.add_argument("--audit", nargs="?", const="", metavar="DIR|FILE",
                    help="numbers of one file or the whole corpus")
    ap.add_argument("--list", metavar="FILE",
                    help="list records with index, offset, length, "
                         "placeholder count and flags")
    ap.add_argument("--accepted", action="store_true",
                    help="--list: only the lines the engine keeps")
    ap.add_argument("--check", metavar="FILE",
                    help="structural check of one kill table")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: also compare byte for byte with FILE")
    ap.add_argument("--set", metavar="FILE",
                    help="replace the text of one record, keeping the layout")
    ap.add_argument("--prepend", metavar="FILE",
                    help="insert one record before --line")
    ap.add_argument("--append", metavar="FILE",
                    help="add one record, keeping the trailing empty line")
    ap.add_argument("--line", type=int, metavar="N",
                    help="1-based record (as --list prints it)")
    ap.add_argument("--text", metavar="TEXT",
                    help="the new message (ASCII, <=2 placeholders, no CR/LF)")
    ap.add_argument("--out", metavar="PATH", help="output file")
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

    modes = [x for x in (args.roundtrip, args.audit, args.list, args.check,
                         args.set, args.prepend, args.append) if x is not None]
    if len(modes) > 1:
        ap.error("choose one of --roundtrip/--audit/--list/--check/--set/"
                 "--prepend/--append")
    if args.accepted and not args.list:
        ap.error("--accepted is a --list option")
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
    if args.prepend:
        args.mode = "prepend"
        args.mode_file = args.prepend
        return _cmd_add(args)
    if args.append:
        args.mode = "append"
        args.mode_file = args.append
        return _cmd_add(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except WriteError as exc:
        print("killedwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, OSError) as exc:
        print("killedwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                           # noqa: BLE001
        print("killedwrite: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
