#!/usr/bin/env python3
"""Writer for the Gunmetal ``.sml`` / ``.sme`` UI markup (``M11.5``, slice 3).

The reader is :mod:`gm.sml` (the oracle, unchanged by this tool).  This module
only *writes*: it decodes/encodes the positional cipher, verifies a document,
lists its elements and replaces the text of one text run while keeping every
other byte (or every other *decoded* byte) intact.

Facts used here (all from ``gm/sml.py`` / ``docs/UI.md`` §3 -- the engine
loader is ``0x497310``, the decoder ``0x497650``):

* **The cipher is positional and byte-based**::

      encoded[i] = letter(plain[i]) ? shift(plain[i], +(i % 10)) : plain[i]

  A letter is shifted forward by ``i % 10`` inside its own alphabet (case
  preserved), every other byte is copied.  Decoding is ``-(i % 10)``.  The
  shift depends on the **byte index**, so inserting or deleting one byte
  re-phases the whole tail; ``decode(encode(x)) == x`` for any byte string,
  which is what makes the codec verifiable byte for byte.
* **``.sme`` is the primary on-disk form**, the ``.sml`` is the plain editor
  form *and* the loader fallback (``0x497310`` appends ``E`` and prefers the
  encoded sibling).
* **Corpus** (``SML/``): 151 ``.sme`` + 149 ``.sml``, 147 stems.  After a
  plain decode **131 pairs are byte-identical**; the other 16 are 14 revisions
  (the ``.sme`` side is a later edit, e.g. ``help.sme`` fixes ``PLayer`` and
  adds a line) and 2 line-ending transports (``prelevel/LEVEL20``,
  ``prelevel/LEVEL4`` were encoded from LF text and shipped with CRLF).  One
  file, ``brief/level17.sme``, carries a stray ``\\r\\n`` at offset 70, so its
  as-is decode is not a valid document; ``gm/sml.py:decode_best`` repairs it
  (note ``newline-repair``).

What this tool does **not** do (see ``docs/SML_WRITE.md`` §5): it never adds,
removes or reorders tags; it never changes attributes, coordinates, fonts,
images or music; it does not lay text out (page layout needs the engine's
glyph metrics -- ``docs/SML_PAGER.md`` §2/§5), so ``--page`` addresses the
``<d>`` document (always 1 in the shipped corpus) and ``--line`` addresses a
*text run*, numbered exactly as ``--list`` prints it.

Usage::

    python3 tools/editor/smlwrite.py --decode brief/level1.sme --out /tmp/opencode/l1.sml
    python3 tools/editor/smlwrite.py --encode /tmp/opencode/l1.sml --out /tmp/opencode/l1.sme
    python3 tools/editor/smlwrite.py --roundtrip                 # game/SML, recursive
    python3 tools/editor/smlwrite.py --roundtrip "$GAME/SML/brief"
    python3 tools/editor/smlwrite.py --check help.sme --json
    python3 tools/editor/smlwrite.py --list brief/level1.sml
    python3 tools/editor/smlwrite.py --set-text help.sml --page 1 --line 4 \
            --text "F3 - Toggle Player ID" --out /tmp/opencode/help.sml
    python3 tools/editor/smlwrite.py --self-test

Exit codes: ``0`` success, ``1`` verification failed (check/roundtrip/
self-test negative), ``2`` bad input or usage (message on stderr), ``3``
internal error.  Nothing is written into the game installation, its mirror or
the repository: ``--out`` is refused there (``WriteError``, code 2).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sml as smlmod                    # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")
DEFAULT_OUT_DIR = "/tmp/opencode/smlwrite"

#: the game installation in use (``--game`` overrides it)
GAME = DEFAULT_GAME

SHIFT_MOD = smlmod.SHIFT_MOD                    # 10
PLAIN_MIN, PLAIN_MAX = 0x20, 0x7E
ALLOWED_CONTROL = (0x09, 0x0A, 0x0D)            # TAB, LF, CR

#: directories that must never receive output: the reference installation,
#: its regression mirror, and this repository.
def guard_roots() -> tuple:
    return (GAME, DEFAULT_GAME, MIRROR_GAME, ROOT)

#: the same tag grammar as ``gm/sml.py:_TAG``, on bytes (latin1 = 1 byte/char)
TAG_BYTES = re.compile(rb"<(/?)([A-Za-z][A-Za-z0-9]*)((?:\s+[^<>]*?)?)(/?)>")

#: shipped-file anomalies that are *not* transport damage (UI.md §4.3)
KNOWN_ANOMALIES = {
    "prelevel/dm4.sml": "no closing </d>; the .SME sibling is well-formed "
                        "and is what the engine loads",
}

SME, SML = ".sme", ".sml"


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
    ext = os.path.splitext(path)[1].lower()
    if ext not in (SME, SML):
        raise WriteError("%s: not a .sml/.sme file (extension %r)"
                         % (path, ext or "<none>"))
    return ext


# ---------------------------------------------------------------------------
# the cipher (an independent implementation; the oracle is gm/sml.py)


def _shift_byte(byte: int, shift: int) -> int:
    if 65 <= byte <= 90:
        return 65 + (byte - 65 + shift) % 26
    if 97 <= byte <= 122:
        return 97 + (byte - 97 + shift) % 26
    return byte


def shift_bytes(data: bytes, sign: int) -> bytes:
    """Positional Caesar; ``sign=+1`` encodes, ``-1`` decodes."""
    out = bytearray(len(data))
    for i, byte in enumerate(data):
        out[i] = _shift_byte(byte, (i % SHIFT_MOD) * sign)
    return bytes(out)


def decode_bytes(data: bytes) -> bytes:
    return shift_bytes(data, -1)


def encode_bytes(data: bytes) -> bytes:
    return shift_bytes(data, +1)


def repair_newlines(data: bytes) -> bytes:
    """Collapse a CR/LF run to the one space it replaced (``level17``)."""
    return re.sub(rb"[\r\n]+", b" ", data)


def repair_crlf(data: bytes) -> bytes:
    """Drop CR bytes (LF-encoded text shipped with CRLF, UI.md §4.2)."""
    return data.replace(b"\r", b"")


def nonprintable(data: bytes) -> list:
    """``[(offset, byte), ...]`` for bytes outside printable ASCII/CRLF/TAB."""
    return [(i, b) for i, b in enumerate(data)
            if not (PLAIN_MIN <= b <= PLAIN_MAX or b in ALLOWED_CONTROL)]


def require_text_bytes(data: bytes, where: str) -> None:
    if not data:
        raise WriteError("%s: empty file" % where)
    bad = nonprintable(data)
    if bad:
        i, b = bad[0]
        raise WriteError(
            "%s: byte 0x%02x at offset %d is outside 0x20..0x7e (broken "
            "encoding; %d such byte(s))" % (where, b, i, len(bad)))


# ---------------------------------------------------------------------------
# document model (the tag grammar is gm/sml.py's)


def tag_letters(data: bytes) -> set:
    return {m.group(2).decode("latin1") for m in TAG_BYTES.finditer(data)
            if not m.group(1)}


def valid_document(data: bytes) -> bool:
    """``<d>...</d>`` with only shipped tag letters -- ``gm/sml.py``'s rule."""
    matches = list(TAG_BYTES.finditer(data))
    if not matches:
        return False
    first, last = matches[0], matches[-1]
    if first.group(1) or first.group(2).lower() != b"d":
        return False
    if not last.group(1) or last.group(2).lower() != b"d":
        return False
    return tag_letters(data) <= smlmod.SHIPPED_TAGS


def text_runs(data: bytes) -> list:
    """Every non-empty text segment between tags (``gm/sml.py:elements`` order).

    Each item is ``{"index", "start", "end", "text"}`` where ``start``/``end``
    delimit the *core* of the run -- leading and trailing whitespace is not
    part of it, so an edit cannot eat the spacing that separates markup.
    """
    runs = []

    def add(start: int, stop: int) -> None:
        segment = data[start:stop]
        core = segment.strip()
        if not core:
            return
        lead = len(segment) - len(segment.lstrip())
        body = core.decode("latin1")
        runs.append({"index": len(runs) + 1, "start": start + lead,
                     "end": start + lead + len(core), "text": body,
                     "seg_start": start})

    last = 0
    for m in TAG_BYTES.finditer(data):
        add(last, m.start())
        last = m.end()
    add(last, len(data))
    return runs


def load_document(path: str, repaired: bool = True) -> dict:
    """Read a ``.sml``/``.sme`` file and decode it for inspection/editing.

    For ``.sme`` the *as-is* positional decode is used for editing and
    round-trip checks; ``note`` (from :func:`gm.sml.decode_best`) says whether
    the file needs a transport repair to be a readable document.  With
    ``repaired`` the readable variant is returned as ``text``.
    """
    kind = kind_of(path)
    raw = read_bytes(path)
    require_text_bytes(raw, path)
    info = {"path": path, "kind": kind, "raw": raw, "note": "as-is"}
    if kind == SME:
        exact = decode_bytes(raw)
        require_text_bytes(exact, path + " (decoded)")
        oracle_text, note = smlmod.decode_best(raw)
        info["exact"] = exact
        info["note"] = note
        if repaired and note != "as-is":
            info["text"] = oracle_text.encode("latin1")
        else:
            info["text"] = exact
    else:
        info["exact"] = raw
        info["text"] = raw
    return info


# ---------------------------------------------------------------------------
# --check


def doc_problems(data: bytes, name: str = "") -> dict:
    """Structural checks of one document (tags, wrapper, brackets, limit)."""
    problems, warnings = [], []
    matches = list(TAG_BYTES.finditer(data))
    letters = tag_letters(data)
    unknown = sorted(letters - smlmod.SHIPPED_TAGS)
    opens = data.count(b"<")
    closes = data.count(b">")
    known = KNOWN_ANOMALIES.get(name.replace("\\", "/").lower())

    if not matches:
        problems.append("no tags at all")
    else:
        first, last = matches[0], matches[-1]
        if first.group(1) or first.group(2).lower() != b"d":
            problems.append("first tag is not <d>")
        if not last.group(1) or last.group(2).lower() != b"d":
            if known:
                warnings.append("known anomaly: %s" % known)
            else:
                problems.append("last tag is not </d>")
    if opens != closes:
        problems.append("bracket imbalance: %d '<' vs %d '>'" % (opens, closes))
    if unknown:
        problems.append("unknown tag letter(s): %s" % ", ".join(unknown))
    if len(matches) >= smlmod.MAX_TAGS:
        problems.append("tag count %d >= limit %d (engine refuses the file)"
                        % (len(matches), smlmod.MAX_TAGS))
    bad = nonprintable(data)
    if bad:
        i, b = bad[0]
        problems.append("byte 0x%02x at offset %d is outside printable ASCII"
                        % (b, i))
    return {"problems": problems, "warnings": warnings, "tags": len(matches),
            "letters": sorted(letters), "unknown": unknown,
            "brackets": [opens, closes], "chars": len(data)}


def _info_from_bytes(raw: bytes, kind: str, path: str) -> dict:
    """The same shape as :func:`load_document`, for an in-memory result."""
    info = {"path": path, "kind": kind, "raw": raw, "note": "as-is"}
    if kind == SME:
        info["exact"] = decode_bytes(raw)
        text, note = smlmod.decode_best(raw)
        info["note"] = note
        info["text"] = text.encode("latin1") if note != "as-is" \
            else info["exact"]
    else:
        info["exact"] = raw
        info["text"] = raw
    return info


def check_info(info: dict, path: str) -> dict:
    """Validate one decoded document; ``info`` comes from load/_info_from_bytes."""
    text = info["text"]
    kind = info["kind"]
    rel = os.path.join(os.path.basename(os.path.dirname(os.path.abspath(path))),
                       os.path.basename(path))
    rep = {
        "tool": "smlwrite", "mode": "check", "file": path, "kind": kind,
        "bytes": len(info["raw"]), "sha256": sha256(info["raw"]),
        "note": info["note"],
    }
    summary = smlmod.describe(text.decode("latin1"))
    dp = doc_problems(text, rel)
    rep.update({k: dp[k] for k in ("tags", "letters", "brackets")})
    rep["unknown_tags"] = summary["unknown_tags"]
    rep["tag_counts"] = summary["tag_counts"]
    rep["doc"] = summary["doc"]
    rep["text_runs"] = summary["text_runs"]
    rep["images"] = len(summary["images"])
    rep["fonts"] = len(summary["fonts"])
    rep["music"] = len(summary["music"])
    problems = list(dp["problems"])
    warnings = list(dp["warnings"])
    if info["note"] not in ("as-is",) and kind == SME:
        warnings.insert(0, "transport repair applied: %s" % info["note"])
    if kind == SME:
        if encode_bytes(info["exact"]) != info["raw"]:
            problems.append("re-encode drift: encode(decode(file)) != file")
        if info["note"] == "unrepaired":
            problems.append("no decode variant yields a <d>...</d> document")
    rep["problems"] = problems
    rep["warnings"] = warnings
    rep["ok"] = not problems
    return rep


def check_file(path: str) -> dict:
    """``--check``: decode (repairing a transport anomaly), then validate."""
    return check_info(load_document(path, repaired=True), path)



def print_check(rep) -> None:
    state = "ok" if rep["ok"] else "FAILED"
    print("check: %s %s (%s, %d bytes, %d tags, %d text runs)"
          % (rep["file"], state, rep["kind"], rep["bytes"], rep["tags"],
             rep["text_runs"]))
    if rep["note"] != "as-is":
        print("  decode: %s" % rep["note"])
    if rep["doc"]["ok"]:
        print("  wrapper: <d>...</d> ok, brackets %d/%d, images %d, fonts %d, "
              "music %d" % (rep["brackets"][0], rep["brackets"][1],
                            rep["images"], rep["fonts"], rep["music"]))
    for w in rep["warnings"]:
        print("  warning: %s" % w)
    for p in rep["problems"]:
        print("  - %s" % p)


# ---------------------------------------------------------------------------
# --list


def list_elements(path: str) -> dict:
    info = load_document(path, repaired=True)
    text = info["text"].decode("latin1")
    elements = smlmod.elements(text)
    runs = text_runs(info["text"])
    run_of_segment = {r["seg_start"]: r["index"] for r in runs}
    rows = []
    for el in elements:
        row = el.as_dict()
        # ``elements`` reports the start of the raw text segment in ``pos``
        row["run"] = run_of_segment.get(el.pos) if el.kind == "text" else None
        rows.append(row)
    return {"tool": "smlwrite", "mode": "list", "file": path,
            "kind": info["kind"], "bytes": len(info["raw"]),
            "note": info["note"], "text_runs": len(runs), "elements": rows}


def print_list(rep) -> None:
    print("list: %s (%s, %d bytes, %d elements, %d text runs%s)"
          % (rep["file"], rep["kind"], rep["bytes"], len(rep["elements"]),
             rep["text_runs"],
             ", decode " + rep["note"] if rep["note"] != "as-is" else ""))
    for i, row in enumerate(rep["elements"], 1):
        extra = []
        if row["x"] is not None or row["y"] is not None:
            extra.append("x=%s y=%s" % (row["x"], row["y"]))
        if row["align"]:
            extra.append("align=%s" % row["align"])
        if row["track"] is not None:
            extra.append("track=%s" % row["track"])
        if row["offset"] is not None:
            extra.append("offset=%s" % row["offset"])
        if row["run"] is not None:
            extra.append("run=%d" % row["run"])
        body = row["value"] if row["value"] is not None else ""
        if row["kind"] == "text":
            body = '"%s"' % (row["text"][:70] + ("..." if len(row["text"]) > 70
                                                else ""))
        print("  %3d %-12s line=%-2d pos=%-5d %s"
              % (i, row["kind"], row["line"], row["pos"],
                 " ".join(extra + [str(body)])))


# ---------------------------------------------------------------------------
# text edit


def _validate_new_text(new: str) -> bytes:
    if not new or not new.strip():
        raise WriteError("--text must not be empty")
    data = new.encode("latin1", "strict") if _latin1_ok(new) else None
    if data is None:
        raise WriteError("--text must be printable ASCII (latin1)")
    for i, b in enumerate(data):
        if not (PLAIN_MIN <= b <= PLAIN_MAX):
            raise WriteError("--text contains byte 0x%02x at offset %d; only "
                             "printable ASCII 0x20..0x7e is allowed" % (b, i))
    for ch in "<>":
        if ch in new:
            raise WriteError("--text must not contain %r: it would change the "
                             "markup structure" % ch)
    if "\r" in new or "\n" in new:
        raise WriteError("--text must not contain CR/LF: a markup line is one "
                         "text run; line breaks come from <r>/<p> tags")
    return data


def _latin1_ok(text: str) -> bool:
    try:
        text.encode("latin1")
        return True
    except UnicodeEncodeError:
        return False


def diff_stats(a: bytes, b: bytes) -> dict:
    """Byte diff: total changed positions and the first differing offset."""
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


def same_structure(old_text: bytes, new_text: bytes) -> bool:
    """The ordered tag/element skeleton is untouched by a text edit."""
    def skeleton(data):
        return [(el.kind, el.tag) for el in
                smlmod.elements(data.decode("latin1")) if el.kind != "text"]
    return skeleton(old_text) == skeleton(new_text)


def set_text(path: str, page: int, line: int, new: str, out: str) -> dict:
    """Replace the text of one run; markup, attributes and encoding stay."""
    guard_out(out, path)
    info = load_document(path, repaired=False)
    kind = info["kind"]
    raw = info["raw"]
    text = info["exact"]
    if kind == SME and info["note"] != "as-is":
        raise WriteError(
            "%s needs the %r transport repair to decode as a document; edit "
            "it through a .sml: --decode %s --repair %s --out X.sml, edit, "
            "then --encode" % (path, info["note"], path,
                               "crlf" if info["note"] == "crlf-strip"
                               else "newline"))
    if not valid_document(text):
        raise WriteError("%s: decoded text is not a <d>...</d> document"
                         % path)
    if page != 1:
        raise WriteError(
            "--page %d: this file has 1 <d> document; pager pages are a "
            "layout result (glyph metrics, docs/SML_PAGER.md §2/§5) and are "
            "not produced by the writer" % page)
    runs = text_runs(text)
    if line < 1 or line > len(runs):
        raise WriteError("--line %d is out of range: the document has %d text "
                         "run(s)" % (line, len(runs)))
    run = runs[line - 1]
    body = _validate_new_text(new)
    if body == run["text"].encode("latin1"):
        raise WriteError("--text equals the current text of run %d (%r)"
                         % (line, run["text"]))
    new_text = text[:run["start"]] + body + text[run["end"]:]
    require_text_bytes(new_text, "edited document")
    if not valid_document(new_text):
        raise WriteError("the edit would break the <d>...</d> document")
    if not same_structure(text, new_text):
        raise WriteError("the edit would change the tag structure (internal)")

    if kind == SME:
        out_bytes = encode_bytes(new_text)
    else:
        out_bytes = new_text
    require_text_bytes(out_bytes, "output")

    # proof 1: the encoded file decodes back to exactly the edited text
    if kind == SME and decode_bytes(out_bytes) != new_text:
        raise WriteError("internal: encode/decode is not an involution")

    # proof 2: everything outside the edited run is untouched
    tail_old = raw[run["end"]:]
    tail_new = out_bytes[run["start"] + len(body):]
    prefix_same = raw[:run["start"]] == out_bytes[:run["start"]]
    suffix_same = tail_old == tail_new
    if kind == SML and not (prefix_same and suffix_same):
        raise WriteError("internal: an .sml edit changed bytes outside the run")

    # proof 3: the oracle sees the new text and the file passes --check
    if kind == SME:
        oracle_text, oracle_note = smlmod.decode_best(out_bytes)
    else:
        oracle_text, oracle_note = out_bytes.decode("latin1"), "as-is"
    expected = new_text.decode("latin1")
    if oracle_text != expected:
        raise WriteError("internal: gm/sml.py decode_best does not see the "
                         "new text")
    check = check_info(_info_from_bytes(out_bytes, kind, out), out)
    if not check["ok"]:
        raise WriteError("internal: the edited file does not pass --check: %s"
                         % "; ".join(check["problems"]))
    rep = {
        "tool": "smlwrite", "mode": "set-text", "file": path, "kind": kind,
        "page": page, "line": line, "runs": len(runs),
        "old_text": run["text"], "new_text": new, "out": os.path.abspath(out),
        "run_span": [run["start"], run["end"]],
        "bytes_in": len(raw), "bytes_out": len(out_bytes),
        "delta": len(out_bytes) - len(raw),
        "old_run_len": len(run["text"]), "new_run_len": len(new),
        "sha256_in": sha256(raw), "sha256_out": sha256(out_bytes),
        "decode_note": oracle_note,
        "structure_same": True,
    }
    stat = diff_stats(raw, out_bytes)
    rep.update({"changed_bytes": stat["changed"],
                "first_diff": stat["first_diff"],
                "prefix_identical": prefix_same,
                "suffix_identical": suffix_same})
    tail_stat = diff_stats(tail_old, tail_new)
    rep["changed_outside_run"] = (
        (0 if prefix_same else stat["changed"]) +
        (0 if suffix_same else tail_stat["changed"]))
    if kind == SME:
        rep["tail_reciphered"] = not suffix_same
    rep["check_ok"] = check["ok"]
    rep["check_problems"] = check["problems"]
    rep["check_warnings"] = check["warnings"]
    write_bytes(out, out_bytes)
    if sha256(read_bytes(out)) != rep["sha256_out"]:
        raise WriteError("internal: %s on disk does not match the report" % out)
    return rep


def print_set_text(rep) -> None:
    print("edit: %s -> %s (%s, page %d, text run %d/%d)"
          % (rep["file"], rep["out"], rep["kind"], rep["page"], rep["line"],
             rep["runs"]))
    print("  text: %r -> %r" % (rep["old_text"], rep["new_text"]))
    print("  bytes: %d -> %d (%+d); run %d -> %d chars; changed %d byte(s), "
          "first at %s"
          % (rep["bytes_in"], rep["bytes_out"], rep["delta"],
             rep["old_run_len"], rep["new_run_len"], rep["changed_bytes"],
             rep["first_diff"]))
    print("  outside the run: prefix identical=%s, suffix identical=%s"
          % ("yes" if rep["prefix_identical"] else "no",
             "yes" if rep["suffix_identical"] else "no"))
    if rep.get("tail_reciphered"):
        print("  note: .sme tail re-ciphered (%d byte(s) differ) -- the shift "
              "depends on the byte index; decode(out) == edited text"
              % rep["changed_outside_run"])
    print("  structure: tags unchanged=%s; gm/sml.py decode=%s; --check: %s"
          % ("yes" if rep["structure_same"] else "NO",
             rep["decode_note"], "ok" if rep["check_ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --decode / --encode


def decode_file(path: str, out: str, repair: str = "none") -> dict:
    guard_out(out, path)
    kind = kind_of(path)
    if kind != SME:
        raise WriteError("%s is not a .sme file (decode expects the encoded "
                         "form)" % path)
    raw = read_bytes(path)
    require_text_bytes(raw, path)
    note = "as-is"
    if repair == "auto":
        text, note = smlmod.decode_best(raw)
        data = text.encode("latin1")
    elif repair == "none":
        data = decode_bytes(raw)
    elif repair == "newline":
        data = decode_bytes(repair_newlines(raw))
        note = "newline-repair"
    elif repair == "crlf":
        data = decode_bytes(repair_crlf(raw))
        note = "crlf-strip"
    else:
        raise WriteError("unknown --repair mode %r" % repair)
    require_text_bytes(data, path + " (decoded)")
    sibling = _sibling(path, SML, exclude=out)
    rep = {
        "tool": "smlwrite", "mode": "decode", "file": path, "out": out,
        "repair": repair, "note": note, "bytes_in": len(raw),
        "bytes_out": len(data), "sha256": sha256(data),
        "doc_ok": valid_document(data),
        "sibling": sibling,
        "sibling_identical": (read_bytes(sibling) == data) if sibling else None,
    }
    warn = None
    if repair == "none" and not valid_document(data):
        _text, best_note = smlmod.decode_best(raw)
        if best_note == "unrepaired":
            warn = ("the exact decode is not a <d>...</d> document and no "
                    "decode variant repairs it: the file is malformed, not "
                    "just transported with the wrong line endings")
        else:
            warn = ("the exact decode is not a <d>...</d> document; "
                    "gm/sml.py suggests %r -- use --repair %s for the readable "
                    "form (the exact codec output is written)"
                    % (best_note, "crlf" if best_note == "crlf-strip"
                       else "newline"))
    rep["warning"] = warn
    write_bytes(out, data)
    return rep


def encode_file(path: str, out: str) -> dict:
    guard_out(out, path)
    kind = kind_of(path)
    if kind != SML:
        raise WriteError("%s is not a .sml file (encode expects the plain "
                         "form)" % path)
    plain = read_bytes(path)
    require_text_bytes(plain, path)
    if not valid_document(plain):
        raise WriteError("%s is not a <d>...</d> document; refusing to encode "
                         "(run --check for the reason)" % path)
    data = encode_bytes(plain)
    require_text_bytes(data, "encoded output")
    if decode_bytes(data) != plain:
        raise WriteError("internal: encode/decode is not an involution")
    sibling = _sibling(path, SME, exclude=out)
    rep = {
        "tool": "smlwrite", "mode": "encode", "file": path, "out": out,
        "bytes_in": len(plain), "bytes_out": len(data),
        "sha256": sha256(data), "sibling": sibling,
        "sibling_identical": (read_bytes(sibling) == data) if sibling else None,
        "sibling_bytes": len(read_bytes(sibling)) if sibling else None,
    }
    write_bytes(out, data)
    return rep


def print_decode(rep) -> None:
    sib = rep["sibling"]
    tail = "" if not sib else (
        "; sibling %s: %s" % (os.path.basename(sib),
                              "identical" if rep["sibling_identical"]
                              else "differs"))
    print("decode: %s -> %s (%d -> %d bytes, %s, doc=%s%s)"
          % (rep["file"], rep["out"], rep["bytes_in"], rep["bytes_out"],
             rep["note"], "ok" if rep["doc_ok"] else "NOT A DOCUMENT", tail))
    if rep["warning"]:
        print("  warning: %s" % rep["warning"])


def print_encode(rep) -> None:
    sib = rep["sibling"]
    tail = "" if not sib else (
        "; sibling %s: %s" % (os.path.basename(sib),
                              "identical (%d B)" % rep["sibling_bytes"]
                              if rep["sibling_identical"] else "differs"))
    print("encode: %s -> %s (%d -> %d bytes%s)"
          % (rep["file"], rep["out"], rep["bytes_in"], rep["bytes_out"], tail))


def _sibling(path: str, want: str, exclude: str | None = None) -> str | None:
    """The same-stem file with extension *want* in the same directory."""
    stem = os.path.splitext(path)[0]
    skip = {os.path.realpath(path)}
    if exclude:
        skip.add(os.path.realpath(exclude))
    for ext in (want, want.upper()):
        candidate = stem + ext
        if os.path.exists(candidate) and os.path.realpath(candidate) not in skip:
            return candidate
    return None


# ---------------------------------------------------------------------------
# --roundtrip


def sml_files(directory: str) -> list:
    """Every ``.sml``/``.sme`` under *directory* (recursive, case-insensitive)."""
    out = []
    for base, _dirs, names in os.walk(directory):
        for name in names:
            if os.path.splitext(name)[1].lower() in (SME, SML):
                out.append(os.path.join(base, name))
    return sorted(out)


def roundtrip_corpus(directory: str = "", verbose: bool = False) -> dict:
    if not directory:
        directory = os.path.join(GAME, "SML")
    if not os.path.isdir(directory):
        raise WriteError("%s is not a directory" % directory)
    files = sml_files(directory)
    smes = [p for p in files if p.lower().endswith(SME)]
    smls = [p for p in files if p.lower().endswith(SML)]
    if not smes:
        raise WriteError("no .sme files under %s" % directory)
    by_stem = {os.path.splitext(p)[0].lower(): p for p in smls}

    started = time.time()
    rows = []
    counts = {"identical": 0, "revision": 0, "line-endings": 0,
              "no-sml": 0, "error": 0}
    totals = {"sme": 0, "sml": 0}
    reenc_ok = cipher_ok = doc_ok = pair_reenc = 0
    for path in smes:
        rel = os.path.relpath(path, directory)
        raw = read_bytes(path)
        totals["sme"] += len(raw)
        bad = nonprintable(raw)
        row = {"file": rel.replace(os.sep, "/"), "bytes_sme": len(raw),
               "problems": []}
        if bad:
            counts["error"] += 1
            row["status"] = "error"
            row["problems"].append(
                "byte 0x%02x at offset %d is outside 0x20..0x7e"
                % (bad[0][1], bad[0][0]))
            rows.append(row)
            continue
        exact = decode_bytes(raw)
        oracle = smlmod.decode(raw)
        if exact != oracle:
            row["problems"].append("cipher differs from gm/sml.py")
        else:
            cipher_ok += 1
        if encode_bytes(exact) == raw:
            reenc_ok += 1
        else:
            row["problems"].append("encode(decode(.sme)) != .sme")
        text, note = smlmod.decode_best(raw)
        row["note"] = note
        if note != "unrepaired":
            doc_ok += 1
        else:
            row["problems"].append("no decode variant is a document")
        sib = by_stem.get(os.path.splitext(path)[0].lower())
        if sib is None:
            row["status"] = "no-sml"
            counts["no-sml"] += 1
        else:
            sib_raw = read_bytes(sib)
            totals["sml"] += len(sib_raw)
            row.update({"sml": os.path.relpath(sib, directory).replace(os.sep, "/"),
                        "bytes_sml": len(sib_raw)})
            if exact == sib_raw:
                row["status"] = "identical"
                counts["identical"] += 1
                if encode_bytes(sib_raw) == raw:
                    pair_reenc += 1
                else:
                    row["problems"].append(
                        "encode(sibling .sml) != .sme")
            elif (decode_bytes(repair_crlf(raw)).replace(b"\r", b"") ==
                  sib_raw.replace(b"\r", b"")):
                # same text, different line endings: the .sme was encoded
                # from LF text and shipped with CRLF (UI.md §4.2)
                row["status"] = "line-endings"
                counts["line-endings"] += 1
                row["reason"] = ("CRLF: .sme encoded from LF text and shipped "
                                 "with CRLF (crlf-strip)")
            else:
                row["status"] = "revision"
                counts["revision"] += 1
                if note == "newline-repair":
                    row["reason"] = ("level17: stray CRLF inside the file "
                                     "(newline-repair)")
                else:
                    row["reason"] = ("revision: .sme %d B vs .sml %d B"
                                     % (len(raw), len(sib_raw)))
        rows.append(row)
        if verbose:
            print("  %-30s %-13s %6d B  %s"
                  % (row["file"], row["status"], row["bytes_sme"],
                     row["note"]))
    pairs = len(smes) - counts["no-sml"] - counts["error"]
    rep = {
        "tool": "smlwrite", "mode": "roundtrip", "dir": os.path.abspath(directory),
        "sme": len(smes), "sml": len(smls), "pairs": pairs,
        "identical": counts["identical"], "revisions": counts["revision"],
        "line_endings": counts["line-endings"],
        "without_sml": counts["no-sml"], "errors": counts["error"],
        "cipher_matches_oracle": cipher_ok, "reencode_identical": reenc_ok,
        "document_decodes": doc_ok, "pair_reencode_identical": pair_reenc,
        "bytes_sme": totals["sme"], "bytes_sml": totals["sml"],
        "seconds": round(time.time() - started, 2),
        "problems": [r for r in rows if r.get("problems")],
        "results": rows,
    }
    rep["ok"] = (not rep["problems"] and cipher_ok == len(smes)
                 and reenc_ok == len(smes) and doc_ok == len(smes)
                 and pair_reenc == counts["identical"])
    return rep


def print_roundtrip(rep) -> None:
    print("roundtrip: %s" % rep["dir"])
    print("  %d .sme, %d .sml, %d pairs, %.2fs; bytes .sme %d, .sml %d"
          % (rep["sme"], rep["sml"], rep["pairs"], rep["seconds"],
             rep["bytes_sme"], rep["bytes_sml"]))
    print("  identical=%d/%d  revisions=%d  line-endings=%d  without .sml=%d"
          % (rep["identical"], rep["pairs"], rep["revisions"],
             rep["line_endings"], rep["without_sml"]))
    print("  re-encode .sme: %d/%d byte-identical; cipher vs gm/sml.py: %d/%d"
          % (rep["reencode_identical"], rep["sme"],
             rep["cipher_matches_oracle"], rep["sme"]))
    print("  pair re-encode (sibling .sml -> .sme): %d/%d identical"
          % (rep["pair_reencode_identical"], rep["identical"]))
    print("  document decodes (3 repair variants): %d/%d"
          % (rep["document_decodes"], rep["sme"]))
    diffs = [r for r in rep["results"] if r["status"] != "identical"]
    if diffs:
        print("  not byte-identical (%d):" % len(diffs))
        for row in diffs:
            reason = row.get("reason")
            if not reason and row["status"] == "line-endings":
                reason = ("CRLF: .sme encoded from LF text and shipped with "
                          "CRLF (crlf-strip)")
            if not reason and row["status"] == "no-sml":
                reason = "no .sml sibling in the corpus"
            print("    %-28s %-13s %s"
                  % (row["file"], row["status"], reason or ""))
    for row in rep["problems"]:
        print("  problem: %s: %s" % (row["file"], "; ".join(row["problems"])))
    print("result: %s" % ("OK" if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# self-test (no game files)


def _synth_doc() -> bytes:
    return (b'<d><r><f s="2">Call</f><s><p>Morning, <k>. Lieutenant.\r\n'
            b'<r><c>F1 - Help</c><c>F2 - Pause</c><I x="64" y="64" '
            b'f="ltmills" a="l"><r><c>Dispatch out</c></d>')


def _selftest() -> bool:
    results = []
    work = tempfile.mkdtemp(prefix="smlwrite-selftest-")

    def case(name, fn):
        try:
            detail = fn()
            results.append((name, True, detail or ""))
        except AssertionError as exc:
            results.append((name, False, "assertion: %s" % exc))
        except WriteError as exc:
            results.append((name, False, "WriteError: %s" % exc))
        except Exception as exc:                       # noqa: BLE001
            results.append((name, False, "%s: %s" % (type(exc).__name__, exc)))

    def tmpfile(name: str, data: bytes) -> str:
        return _write(work, name, data)

    def expect_error(fn, needle, label):
        try:
            fn()
        except WriteError as exc:
            if needle not in str(exc):
                raise AssertionError("%s: message %r lacks %r"
                                     % (label, str(exc), needle))
            return str(exc)
        raise AssertionError("%s: no WriteError raised" % label)

    plain = _synth_doc()
    encoded = encode_bytes(plain)

    def t_shift():
        data = b"AZaz09 <d>"
        enc = encode_bytes(data)
        assert enc[0] == ord("A"), enc[:1]         # index 0: no shift
        assert enc[1] == ord("A"), enc[1:2]        # index 1: Z+1 wraps to A
        assert enc[2] == ord("c"), enc[2:3]        # index 2: a+2 -> c
        assert enc[6] == ord(" "), enc[6:7]         # non-letters are copied
        assert enc[7] == ord("<"), enc[7:8]
        return "A+0, Z+1->A, a+2->c, punctuation copied"

    def t_involution():
        assert decode_bytes(encode_bytes(plain)) == plain
        assert encode_bytes(decode_bytes(plain)) == plain
        for n in (1, 7, 26, 100, 511):
            blob = bytes((i * 37 + 11) % 256 for i in range(n))
            assert decode_bytes(encode_bytes(blob)) == blob
        return "5 sizes, decode/encode commute"

    def t_case():
        text = bytes(range(65, 91)) + bytes(range(97, 123))
        for i, b in enumerate(encode_bytes(text)):
            src = text[i]
            if 65 <= src <= 90:
                assert 65 <= b <= 90, (i, src, b)
            else:
                assert 97 <= b <= 122, (i, src, b)
        return "52 letters stay in their own alphabet"

    def t_mod10():
        # 26 letters at consecutive indices: the shift cycles 0..9, 0..9, ...
        text = bytes(range(65, 91))
        enc = encode_bytes(text)
        for i in range(26):
            assert enc[i] == 65 + ((text[i] - 65 + i % 10) % 26), i
        return "shift == i %% 10 (26 letters)"

    def t_oracle_parity():
        assert decode_bytes(encoded) == smlmod.decode(encoded)
        assert encode_bytes(plain) == smlmod.encode(plain)
        blob = bytes((i * 53) % 256 for i in range(300))
        assert decode_bytes(blob) == smlmod.decode(blob)
        return "gm/sml.py decode/encode agree on 2 docs and 300 random bytes"

    def t_crlf_kept():
        dec = decode_bytes(encoded)
        assert b"\r\n" in dec, "the CRLF of the synthetic doc was lost"
        i = dec.index(b"\r\n")
        assert dec[i:i + 2] == b"\r\n", dec[i:i + 2]
        return "CR/LF stay at their byte offsets"

    def t_crlf_anomaly():
        lf = plain.replace(b"\r\n", b"\n")
        sme = encode_bytes(lf).replace(b"\n", b"\r\n")
        assert not valid_document(decode_bytes(sme)), "as-is decode was valid"
        assert repair_crlf(sme) != sme
        fixed = decode_bytes(repair_crlf(sme))
        assert fixed == lf, (len(fixed), len(lf))
        assert valid_document(fixed)
        return "LF-encoded + CRLF-transported: as-is broken, crlf-strip ok"

    flat = plain.replace(b"\r\n", b" ")     # no legitimate CR/LF: level17 shape

    def _anomaly(base: bytes) -> tuple:
        """Insert a stray CRLF where *base* holds a space (the level17 shape)."""
        pos = base.index(b" ", 30)
        broken = base[:pos] + b"\r\n" + base[pos + 1:]
        assert len(broken) == len(base) + 1
        assert repair_newlines(broken) == base, "repair does not collapse"
        return pos, broken

    def t_level17():
        base = encode_bytes(flat)
        pos, broken = _anomaly(base)
        assert not valid_document(decode_bytes(broken)), "as-is decode was valid"
        fixed = decode_bytes(repair_newlines(broken))
        assert len(fixed) == len(flat), (len(fixed), len(flat))
        assert valid_document(fixed)
        assert fixed == flat, "repair did not restore the document"
        return ("extra CRLF at offset %d: as-is invalid, newline-repair "
                "restores" % pos)

    def t_nonprintable():
        expect_error(lambda: require_text_bytes(b"<d>\x01</d>", "x"),
                     "0x01", "control byte")
        expect_error(lambda: require_text_bytes(b"<d>\xe9</d>", "x"),
                     "0xe9", "high byte")
        expect_error(lambda: require_text_bytes(b"", "x"), "empty", "empty")

    def t_bad_ext():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            txt = _write(tmp, "a.txt", b"<d></d>")
            expect_error(lambda: kind_of(txt), "not a .sml/.sme", "wrong ext")
            expect_error(lambda: decode_file(txt, os.path.join(tmp, "o.sml")),
                         "not a .sml/.sme", "decode ext")

    def t_empty_file():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            empty_sml = _write(tmp, "e.sml", b"")
            expect_error(lambda: check_file(empty_sml), "empty file",
                         "empty .sml")
            empty_sme = _write(tmp, "e.sme", b"")
            expect_error(lambda: decode_file(empty_sme,
                                             os.path.join(tmp, "o.sml")),
                         "empty file", "empty .sme")

    def t_decode_ext():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            sml = _write(tmp, "a.sml", plain)
            expect_error(lambda: decode_file(sml, os.path.join(tmp, "o.sme")),
                         "not a .sme", "decode on .sml")

    def t_encode_ext():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            sme = _write(tmp, "a.sme", encoded)
            expect_error(lambda: encode_file(sme, os.path.join(tmp, "o.sml")),
                         "not a .sml", "encode on .sme")

    def t_guard_game():
        expect_error(lambda: guard_out(os.path.join(DEFAULT_GAME, "SML/x.sml")),
                     "refusing to write", "game dir")
        expect_error(lambda: guard_out(os.path.join(MIRROR_GAME, "SML/x.sml")),
                     "refusing to write", "mirror")

    def t_guard_repo():
        expect_error(lambda: guard_out(os.path.join(ROOT, "out/x.sml")),
                     "refusing to write", "repository")

    def t_guard_same():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sml", plain)
            expect_error(lambda: guard_out(src, src), "overwrite the input",
                         "same path")

    def t_decode_encode_files():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            sme = _write(tmp, "level1.sme", encoded)
            dec_path = os.path.join(tmp, "level1.sml")
            rep = decode_file(sme, dec_path)
            assert rep["bytes_out"] == len(plain), rep
            assert rep["doc_ok"] and rep["note"] == "as-is", rep
            enc_path = os.path.join(tmp, "back.sme")
            rep2 = encode_file(dec_path, enc_path)
            assert read_bytes(enc_path) == encoded, "encode is not the inverse"
            assert rep2["sibling"] == sme and rep2["sibling_identical"]
            assert rep2["sibling_identical"], rep2
            return "%d B: .sme -> .sml -> .sme identical" % len(encoded)

    def t_decode_repair():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            lf = plain.replace(b"\r\n", b"\n")
            sme = _write(tmp, "level20.sme",
                         encode_bytes(lf).replace(b"\n", b"\r\n"))
            out = os.path.join(tmp, "a.sml")
            rep = decode_file(sme, out, repair="crlf")
            assert rep["note"] == "crlf-strip" and rep["doc_ok"], rep
            assert read_bytes(out) == lf
            rep2 = decode_file(sme, os.path.join(tmp, "b.sml"))
            assert rep2["warning"], "the exact decode of the anomaly was not "
            "flagged"
            assert os.path.exists(os.path.join(tmp, "b.sml"))
            return "crlf repair readable; exact decode warns"

    def t_check_ok():
        rep = check_file(tmpfile("good.sml", plain))
        assert rep["ok"], rep["problems"]
        assert rep["tags"] == 17, rep["tags"]
        assert rep["text_runs"] == 6, rep["text_runs"]
        return "17 tags, 6 text runs"

    def t_check_wrapper():
        rep = check_file(tmpfile("bad.sml", plain.replace(b"</d>", b"")))
        assert not rep["ok"]
        assert any("</d>" in p for p in rep["problems"]), rep["problems"]

    def t_check_unknown():
        rep = check_file(tmpfile("bad.sml", b"<d><q>text</q></d>"))
        assert not rep["ok"]
        assert any("unknown tag" in p for p in rep["problems"]), rep

    def t_check_brackets():
        rep = check_file(tmpfile("bad.sml", b"<d><r>x</d"))
        assert not rep["ok"]
        assert any("imbalance" in p for p in rep["problems"]), rep["problems"]

    def t_check_limit():
        many = b"<d>" + b"<r>" * (smlmod.MAX_TAGS - 1) + b"</d>"
        rep = check_file(tmpfile("many.sml", many))
        assert not rep["ok"]
        assert any("limit" in p for p in rep["problems"]), rep["problems"]

    def t_check_known():
        bad = plain.replace(b"</d>", b"")
        path = os.path.join("prelevel", "dm4.sml")
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            os.makedirs(os.path.join(tmp, "prelevel"))
            full = _write(os.path.join(tmp, "prelevel"), "dm4.sml", bad)
            rep = check_file(full)
            assert rep["ok"], rep["problems"]
            assert rep["warnings"], "known anomaly not reported"
        return "dm4-style unclosed doc is a warning, not a failure (%s)" % path

    def t_check_sme_note():
        _pos, broken = _anomaly(encode_bytes(flat))
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            rep = check_file(_write(tmp, "level17.sme", broken))
            assert rep["ok"], rep["problems"]
            assert rep["note"] == "newline-repair", rep["note"]
            assert any("transport repair" in w for w in rep["warnings"])

    def t_list():
        rep = list_elements(tmpfile("l.sml", plain))
        kinds = [e["kind"] for e in rep["elements"]]
        assert kinds.count("doc") == 1, kinds
        assert rep["text_runs"] == 6, rep["text_runs"]
        assert [e["run"] for e in rep["elements"] if e["kind"] == "text"] == \
            [1, 2, 3, 4, 5, 6], [e["run"] for e in rep["elements"]]
        img = [e for e in rep["elements"] if e["kind"] == "image"]
        assert len(img) == 1 and img[0]["x"] == 64 and img[0]["y"] == 64, img
        assert img[0]["value"] == "ltmills", img

    def t_set_same_len():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sml", plain)
            out = os.path.join(tmp, "b.sml")
            rep = set_text(src, 1, 4, "F1 - Menu", out)      # 9 -> 9 bytes
            assert rep["delta"] == 0, rep["delta"]
            assert rep["changed_bytes"] == 3, rep["changed_bytes"]
            assert rep["prefix_identical"] and rep["suffix_identical"], rep
            assert rep["changed_outside_run"] == 0, rep
            assert rep["check_ok"], rep["check_problems"]
            assert rep["structure_same"]
            return "%d byte(s) changed, all inside the run" % \
                rep["changed_bytes"]

    def t_set_longer():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sml", plain)
            out = os.path.join(tmp, "b.sml")
            rep = set_text(src, 1, 4, "F1 - Help me", out)
            assert rep["delta"] == 3, rep["delta"]      # 9 -> 12
            assert rep["prefix_identical"] and rep["suffix_identical"], rep
            assert rep["changed_outside_run"] == 0, rep
            assert rep["check_ok"], rep["check_problems"]
            new = read_bytes(out)
            assert b"F1 - Help me</c>" in new
            assert new[:rep["run_span"][0]] == plain[:rep["run_span"][0]]

    def t_set_shorter():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sml", plain)
            out = os.path.join(tmp, "b.sml")
            rep = set_text(src, 1, 5, "F2", out)        # "F2 - Pause" -> "F2"
            assert rep["delta"] == -8, rep["delta"]
            assert rep["suffix_identical"] and rep["prefix_identical"], rep
            assert rep["check_ok"], rep["check_problems"]

    def t_set_sme_same():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sme", encoded)
            out = os.path.join(tmp, "b.sme")
            rep = set_text(src, 1, 4, "F1 - Menu", out)
            assert rep["delta"] == 0, rep["delta"]
            assert rep["changed_outside_run"] == 0, rep
            assert not rep.get("tail_reciphered"), rep
            assert rep["check_ok"], rep["check_problems"]
            assert decode_bytes(read_bytes(out)) == \
                plain.replace(b"F1 - Help", b"F1 - Menu")
            return "%d encoded byte(s) changed, decode(out) == edited text" % \
                rep["changed_bytes"]

    def t_set_sme_longer():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sme", encoded)
            out = os.path.join(tmp, "b.sme")
            rep = set_text(src, 1, 4, "F1 - Help me", out)
            assert rep["delta"] == 3, rep["delta"]
            assert rep["tail_reciphered"], rep
            assert rep["changed_outside_run"] > 0, rep
            assert rep["check_ok"], rep["check_problems"]
            dec = decode_bytes(read_bytes(out))
            assert dec == plain.replace(b"F1 - Help", b"F1 - Help me"), dec
            return "tail re-ciphered (%d B), decode(out) == edited text" % \
                rep["changed_outside_run"]

    def t_set_refuse():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sml", plain)
            out = os.path.join(tmp, "o.sml")
            expect_error(lambda: set_text(src, 2, 1, "x", out), "page 2",
                         "page 2")
            expect_error(lambda: set_text(src, 1, 0, "x", out), "out of range",
                         "line 0")
            expect_error(lambda: set_text(src, 1, 99, "x", out), "out of range",
                         "line 99")
            expect_error(lambda: set_text(src, 1, 1, "", out), "empty", "empty")
            expect_error(lambda: set_text(src, 1, 1, "a<b", out),
                         "must not contain", "angle bracket")
            expect_error(lambda: set_text(src, 1, 1, "caf\u00e9", out),
                         "printable ASCII", "non-ascii")
            expect_error(lambda: set_text(src, 1, 4, "F1 - Help", out),
                         "equals the current text", "no-op")

    def t_set_sme_anomaly():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            _pos, broken = _anomaly(encode_bytes(flat))
            src = _write(tmp, "level17.sme", broken)
            expect_error(lambda: set_text(src, 1, 1, "Hi",
                                          os.path.join(tmp, "o.sme")),
                         "transport repair", "anomalous .sme")
            expect_error(lambda: set_text(src, 1, 1, "Hi",
                                          os.path.join(DEFAULT_GAME, "x.sme")),
                         "refusing to write", "game out before decode")

    def t_set_oracle():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            src = _write(tmp, "a.sml", plain)
            out = os.path.join(tmp, "b.sml")
            set_text(src, 1, 4, "F1 - Menu", out)
            text = read_bytes(out).decode("latin1")
            assert "F1 - Menu" in text, text[:200]
            desc = smlmod.describe(text)
            assert desc["tags"] == 17, desc["tags"]
            assert desc["doc"]["ok"]
            # and through the .sme side, read back by the oracle decoder
            sme = _write(tmp, "a.sme", encoded)
            set_text(sme, 1, 4, "F1 - Menu", os.path.join(tmp, "b.sme"))
            decoded, note = smlmod.decode_best(read_bytes(
                os.path.join(tmp, "b.sme")))
            assert note == "as-is" and "F1 - Menu" in decoded, (note, decoded[:80])
            assert smlmod.describe(decoded)["doc"]["ok"]
            return "gm/sml.py sees the new text on both sides, 17 tags"

    def t_roundtrip_synth():
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            _write(tmp, "level1.sme", encoded)
            _write(tmp, "level1.sml", plain)
            # level2: encoded from a different revision
            _write(tmp, "level2.sme", encode_bytes(plain.replace(b"Help",
                                                                 b"Pause")))
            _write(tmp, "level2.sml", plain)
            # level3: no .sml
            _write(tmp, "level3.sme", encoded)
            # level4: LF-encoded, CRLF-transported
            lf = plain.replace(b"\r\n", b"\n")
            _write(tmp, "level4.sme",
                   encode_bytes(lf).replace(b"\n", b"\r\n"))
            _write(tmp, "level4.sml", lf)
            rep = roundtrip_corpus(tmp)
            assert rep["sme"] == 4 and rep["sml"] == 3, (rep["sme"], rep["sml"])
            assert rep["pairs"] == 3
            assert rep["identical"] == 1, rep["identical"]
            assert rep["revisions"] == 1, rep["revisions"]
            assert rep["line_endings"] == 1, rep["line_endings"]
            assert rep["without_sml"] == 1, rep["without_sml"]
            assert rep["reencode_identical"] == 4
            assert rep["cipher_matches_oracle"] == 4
            assert rep["pair_reencode_identical"] == 1
            assert rep["ok"], rep["problems"]

    def t_roundtrip_bad():
        expect_error(lambda: roundtrip_corpus("/nonexistent/sml"),
                     "not a directory", "missing dir")
        with tempfile.TemporaryDirectory(prefix="smlwrite-selftest-") as tmp:
            expect_error(lambda: roundtrip_corpus(tmp), "no .sme", "empty dir")

    checks = [
        ("cipher: letters shift by i % 10, others copied", t_shift),
        ("cipher: encode/decode is an involution (5 sizes)", t_involution),
        ("cipher: case is preserved in both alphabets", t_case),
        ("cipher: shift cycles 0..9 over 26 letters", t_mod10),
        ("cipher: independent implementation == gm/sml.py", t_oracle_parity),
        ("cipher: CR/LF bytes are copied, not normalised", t_crlf_kept),
        ("transport: LF-encoded + CRLF shipped needs crlf-strip", t_crlf_anomaly),
        ("transport: level17 stray CRLF needs newline-repair", t_level17),
        ("refuse: control/high byte, empty input", t_nonprintable),
        ("refuse: not a .sml/.sme", t_bad_ext),
        ("refuse: empty file (check and decode)", t_empty_file),
        ("refuse: --decode on a .sml", t_decode_ext),
        ("refuse: --encode on a .sme", t_encode_ext),
        ("refuse: --out inside the game installation / mirror", t_guard_game),
        ("refuse: --out inside the repository", t_guard_repo),
        ("refuse: --out overwriting the input", t_guard_same),
        ("decode -> encode through files is byte-identical", t_decode_encode_files),
        ("decode --repair crlf: readable; exact decode warns", t_decode_repair),
        ("--check: clean document ok (tags/runs)", t_check_ok),
        ("--check: missing </d> is a problem", t_check_wrapper),
        ("--check: unknown tag letter is a problem", t_check_unknown),
        ("--check: bracket imbalance is a problem", t_check_brackets),
        ("--check: >= 512 tags is a problem", t_check_limit),
        ("--check: known anomaly dm4 is a warning", t_check_known),
        ("--check: .sme transport repair is reported", t_check_sme_note),
        ("--list: elements, coordinates and text runs", t_list),
        ("edit .sml: same length changes only run bytes", t_set_same_len),
        ("edit .sml: longer text grows the file by the delta", t_set_longer),
        ("edit .sml: shorter text shrinks the file by the delta", t_set_shorter),
        ("edit .sme: same length changes only the run's bytes", t_set_sme_same),
        ("edit .sme: longer text re-ciphers the tail", t_set_sme_longer),
        ("edit: page/line/text refusals", t_set_refuse),
        ("edit: anomalous .sme is refused with a hint", t_set_sme_anomaly),
        ("edit: gm/sml.py re-reads the new text", t_set_oracle),
        ("--roundtrip: synthetic corpus classification and counts",
         t_roundtrip_synth),
        ("--roundtrip: missing/empty directory is refused", t_roundtrip_bad),
    ]
    for name, fn in checks:
        case(name, fn)

    shutil.rmtree(work, ignore_errors=True)
    failed = [r for r in results if not r[1]]
    for name, ok, detail in results:
        print("  %-58s %s%s" % (name, "ok" if ok else "FAIL",
                                ("  (" + detail + ")") if detail and not ok
                                else ""))
    print("%d/%d self-tests passed" % (len(results) - len(failed), len(results)))
    return not failed


def _write(directory: str, name: str, data: bytes) -> str:
    path = os.path.join(directory, name)
    write_bytes(path, data)
    return path


# ---------------------------------------------------------------------------
# CLI


def _cmd_decode(args) -> int:
    if not args.out:
        raise WriteError("--decode needs --out OUT.sml")
    rep = decode_file(args.decode, args.out, args.repair)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_decode(rep)
    return 0


def _cmd_encode(args) -> int:
    if not args.out:
        raise WriteError("--encode needs --out OUT.sme")
    rep = encode_file(args.encode, args.out)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_encode(rep)
    return 0


def _cmd_roundtrip(args) -> int:
    rep = roundtrip_corpus(args.roundtrip or "", args.verbose)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_roundtrip(rep)
    return 0 if rep["ok"] else 1


def _cmd_check(args) -> int:
    rep = check_file(args.check)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_check(rep)
    return 0 if rep["ok"] else 1


def _cmd_list(args) -> int:
    rep = list_elements(args.list)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_list(rep)
    return 0


def _cmd_set_text(args) -> int:
    if args.line is None:
        raise WriteError("--set-text needs --line M (a 1-based text run)")
    if args.new_text is None:
        raise WriteError("--set-text needs --text \"...\"")
    if not args.out:
        raise WriteError("--set-text needs --out OUT")
    rep = set_text(args.set_text, args.page, args.line, args.new_text, args.out)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_set_text(rep)
    return 0 if rep["check_ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(
        description="Writer for the Gunmetal .sml/.sme UI markup (M11.5).")
    ap.add_argument("--decode", metavar="IN.sme",
                    help="decode an .sme into the plain .sml form")
    ap.add_argument("--encode", metavar="IN.sml",
                    help="encode a plain .sml into the .sme form")
    ap.add_argument("--roundtrip", nargs="?", const="", metavar="DIR",
                    help="decode/compare/re-encode every .sme (default "
                         "game/SML)")
    ap.add_argument("--check", metavar="FILE",
                    help="validate a .sml/.sme document")
    ap.add_argument("--list", metavar="FILE",
                    help="list the markup elements and text-run numbers")
    ap.add_argument("--set-text", metavar="FILE", dest="set_text",
                    help="replace the text of one run, keeping the markup")
    ap.add_argument("--page", type=int, default=1, metavar="N",
                    help="page (1-based <d> document; always 1 in the corpus)")
    ap.add_argument("--line", type=int, metavar="M",
                    help="1-based text run to edit (as --list prints it)")
    ap.add_argument("--text", metavar="TEXT", dest="new_text",
                    help="the replacement text (printable ASCII, no <>)")
    ap.add_argument("--out", metavar="PATH", help="output file")
    ap.add_argument("--repair", default="none",
                    choices=("none", "auto", "newline", "crlf"),
                    help="--decode: none = exact codec (default), auto = "
                         "gm/sml.py decode_best, newline = level17, crlf = "
                         "LF-encoded/CRLF files")
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
    if args.roundtrip is not None:
        return _cmd_roundtrip(args)
    if args.decode:
        return _cmd_decode(args)
    if args.encode:
        return _cmd_encode(args)
    if args.check:
        return _cmd_check(args)
    if args.list:
        return _cmd_list(args)
    if args.set_text:
        return _cmd_set_text(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except WriteError as exc:
        print("smlwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, OSError) as exc:
        print("smlwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                           # noqa: BLE001
        print("smlwrite: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
