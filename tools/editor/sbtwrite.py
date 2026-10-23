#!/usr/bin/env python3
"""Writer for the Gunmetal ``.sbt`` container (``M11.4``, part 1).

The reader is :mod:`gm.sbt` (the oracle, unchanged by this tool).  This module
only *writes*: it re-serialises a parsed container, and it can replace the
compiled code of one ``SCRIPT`` sub-script with bytes assembled by
:mod:`tools.editor.sbtasm`.

Container facts used here (all taken from ``gm/sbt.py`` / ``docs/FORMATS.md``):

* **84-byte header**, kept verbatim.  ``char magic[80]`` (``b"BINARY SAVE\\0"``
  + zero padding) and ``u32 version`` at ``0x50`` (must be 0) --
  ``gm/sbt.py:17-25``, ``:113-115``, ``:758-767``.
* **No offset/length table.**  After the header comes a bare stream of
  ``u8 type`` + payload; each type's handler consumes exactly its own payload,
  and ``255`` (``END_OF_FILE``) has none and is the last byte -- ``gm/sbt.py``
  module docstring ``:22-25``, ``parse`` ``:768-786``, type table ``:127-145``.
  Therefore *changing a payload's size needs no absolute-offset fixups*: the
  file is simply longer/shorter by the delta.  Only file offsets of the
  following records move.
* **Exact round-trip comes from raw payload bytes.**  ``Sbt.header`` keeps the
  84 header bytes and ``Record.raw`` keeps each payload verbatim; ``to_bytes``
  emits header + type + raw -- ``gm/sbt.py:490``, ``:498-499``, ``:503-515``.
  So opaque/padding bytes (``THING_TYPES`` name tails, ``SOB_TABLE`` stale
  pointers, handle tags, ...) survive without interpretation.
* ``set_payload`` re-encodes one record through ``encode_payload`` and refreshes
  its ``raw``/``size`` -- ``gm/sbt.py:554-558``.
* ``SCRIPT`` (type 11) layout: ``u32 count`` + ``count*40`` things bytes,
  ``u32 n`` + three column-major ``u32[n]`` handle columns, ``u32 m`` +
  ``m`` code bytes, ``u8 flag`` (+ ``9*0x400`` tables if set) --
  ``gm/sbt.py:333-371``, parser ``:692-701``, ``docs/FORMATS.md`` §12/§12.3.
  Column 2 is the **sub-script entry offset** inside the code buffer
  (``docs/FORMATS.md`` §12.4 "Таблица сущностей", column 2 / global
  ``0x616658``); :func:`tools.editor.sbtasm.block_starts` reads it the same
  way.
* Engine limits: ``m <= 0x30000`` (``gm/sbt.py:697``, ``gm/sbtvm.py:338``),
  ``count <= 0xC8``, ``n <= 0x400`` (``gm/sbt.py:693-696``).

What a replacement recalculates (and what it does not):

1. ``SCRIPT.m = len(new_code)`` -- the loader reads ``m`` and then exactly
   ``m`` bytes (``0x462a80``; ``gm/sbt.py:695-700``).
2. every column-2 entry ``>= end`` of the replaced span is shifted by
   ``len(new) - len(old)``, because those are offsets into the same code
   buffer (``docs/FORMATS.md`` §12.4).  An entry strictly inside the span is an
   overlap and is rejected.
3. the record's ``raw``/``size`` (via ``Sbt.set_payload``); the container size
   follows from concatenation -- there is nothing else to patch.
4. nothing else: ``count``, ``n``, ``flag``, the things block, handle columns
   0/1 and the ``DEFINE_*`` tables are asserted unchanged.

Usage::

    python3 tools/editor/sbtwrite.py --roundtrip                 # 36 shipped .sbt
    python3 tools/editor/sbtwrite.py --self-test                 # no game files
    python3 tools/editor/sbtwrite.py --script dm1 --block 0x2b07 \\
            --asm /tmp/opencode/blk.asm --out /tmp/opencode/dm1.sbt --json
    python3 tools/editor/sbtwrite.py --script level0 --block 0x6f92 \\
            --text @/tmp/opencode/blk.asm --out /tmp/opencode/level0.sbt
    python3 tools/editor/sbtwrite.py --check /tmp/opencode/dm1.sbt
    python3 tools/editor/sbtwrite.py --verify-replace            # 3 levels

``--text`` takes inline assembler text; a leading ``@`` means "read this text
file" (convenience, same as ``--asm``).

Exit codes: ``0`` success, ``1`` verification failed (round-trip/check/self-test
negative), ``2`` bad input/usage (message on stderr), ``3`` internal error.
No traceback escapes :func:`main`.
"""
from __future__ import annotations

import argparse
import copy
import glob
import hashlib
import json
import os
import struct
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbt as sbtmod                 # noqa: E402
from gm import sbtvm                         # noqa: E402
from tools.editor import sbtasm              # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sbtwrite"

SCRIPT_TYPE = 11
END = sbtmod.END_OF_FILE
MAX_CODE = sbtvm.MAX_BLOCK                   # 0x30000 (gm/sbtvm.py:338)
MAX_THINGS = 0xC8                            # gm/sbt.py:693
MAX_HANDLES = 0x400                          # gm/sbt.py:695


class WriteError(Exception):
    """Bad input or an impossible edit; message is meant for the user."""


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


def parse_container(data: bytes, where: str):
    """Parse with the oracle; turn its exceptions into :class:`WriteError`."""
    try:
        return sbtmod.parse(data)
    except (ValueError, struct.error) as exc:
        raise WriteError("%s: %s" % (where, exc))


def load_container(path: str):
    data = read_bytes(path)
    return data, parse_container(data, path)


# ---------------------------------------------------------------------------
# structure digests


def record_digests(sbt) -> list:
    out = []
    for i, r in enumerate(sbt.records):
        out.append({
            "index": i,
            "type": r.type,
            "name": r.name,
            "offset": r.offset,
            "size": r.size,
            "raw_len": len(r.raw),
            "sha256": sha256(r.raw),
        })
    return out


def script_columns(script):
    """Split the three parallel handle columns (``gm/sbt.py:692-701``)."""
    n = script.n
    if n == 0:
        if script.handles:
            raise WriteError("SCRIPT: n=0 but %d handle bytes present"
                             % len(script.handles))
        return ([], [], [])
    if len(script.handles) != 12 * n:
        raise WriteError("SCRIPT: handle block is %d bytes, expected %d (n=%d)"
                         % (len(script.handles), 12 * n, n))
    return tuple(list(struct.unpack_from("<%dI" % n, script.handles, 4 * k * n))
                 for k in range(3))


def encode_columns(columns) -> bytes:
    n = len(columns[0])
    return b"".join(struct.pack("<%dI" % n, *col) for col in columns)


def block_spans_of(script, where="SCRIPT"):
    """``[(start, end)]`` of every sub-script (validated, in file order)."""
    try:
        starts = sbtasm.block_starts(script)
        return sbtasm.block_spans(script.code, starts)
    except (ValueError, struct.error) as exc:
        raise WriteError("%s: bad sub-script table: %s" % (where, exc))


def script_digest(script, where="SCRIPT") -> dict:
    spans = block_spans_of(script, where)
    return {
        "count": script.count,
        "n": script.n,
        "m": script.m,
        "flag": script.flag,
        "things_len": len(script.things),
        "handles_len": len(script.handles),
        "tables_len": len(script.tables),
        "code_len": len(script.code),
        "code_sha256": sha256(script.code),
        "blocks": [{"offset": s, "size": e - s,
                    "sha256": sha256(script.code[s:e])} for s, e in spans],
    }


def plan_digest(sbt) -> dict:
    """Everything ``--check --expect`` compares."""
    plan = {"container": {"version": sbt.version,
                          "header_sha256": sha256(sbt.header),
                          "records": record_digests(sbt)}}
    scripts = [r for r in sbt.records if r.type == SCRIPT_TYPE]
    if scripts:
        plan["script"] = script_digest(scripts[0].payload)
    return plan


# ---------------------------------------------------------------------------
# validation


def validate_block(data: bytes, what: str = "block") -> None:
    """Require exactly one well-formed sub-script: bytecode reaching ``END``."""
    if not data:
        raise WriteError("%s is empty (a sub-script needs at least END 0xFF)"
                         % what)
    if data[-1] != END:
        raise WriteError("%s does not end with END (0xFF); last byte is 0x%02x"
                         % (what, data[-1]))
    try:
        insns, end = sbtvm.decode(data)
    except (ValueError, struct.error, IndexError) as exc:
        raise WriteError("%s is not valid SCRIPT bytecode: %s" % (what, exc))
    if not insns or insns[-1].name != "END":
        raise WriteError("%s: decode did not finish on END" % what)
    ends = [k for k, ins in enumerate(insns) if ins.name == "END"]
    if ends != [len(insns) - 1]:
        raise WriteError("%s: %d END (0xFF) byte(s) inside the block; a "
                         "sub-script ends at its first END"
                         % (what, len(ends)))
    if end != len(data):
        raise WriteError("%s: %d trailing byte(s) after the block's END "
                         "(decode stopped at 0x%x of 0x%x)"
                         % (what, len(data) - end, end, len(data)))


def structure_problems(sbt, data: bytes | None = None) -> list:
    """Structural self-consistency of a parsed container (list of strings)."""
    problems = []
    if data is not None:
        try:
            rebuilt = sbt.to_bytes()
        except (ValueError, struct.error) as exc:
            problems.append("to_bytes() failed: %s" % exc)
        else:
            if rebuilt != data:
                diff = next((i for i in range(min(len(rebuilt), len(data)))
                             if rebuilt[i] != data[i]), min(len(rebuilt),
                                                            len(data)))
                problems.append("to_bytes() is not byte-identical to the input "
                                "(%d vs %d bytes, first diff at 0x%x)"
                                % (len(rebuilt), len(data), diff))

    want_off = sbt.header_size
    for i, r in enumerate(sbt.records):
        if r.offset != want_off:
            problems.append("record #%d %s: offset 0x%x, expected 0x%x"
                            % (i, r.name, r.offset, want_off))
        if r.size != len(r.raw) + 1:
            problems.append("record #%d %s: size %d != 1 + %d raw bytes"
                            % (i, r.name, r.size, len(r.raw)))
        want_off += r.size
    if data is not None:
        if want_off != len(data):
            problems.append("record stream ends at 0x%x, file is 0x%x bytes"
                            % (want_off, len(data)))
        if not sbt.records or sbt.records[-1].type != END:
            problems.append("record stream has no END_OF_FILE terminator")

    scripts = [r for r in sbt.records if r.type == SCRIPT_TYPE]
    if len(scripts) > 1:
        problems.append("%d SCRIPT records (type 11); only one is supported"
                        % len(scripts))
    for r in scripts:
        sc = r.payload
        tag = "SCRIPT at 0x%x" % r.offset
        if len(sc.things) != 40 * sc.count:
            problems.append("%s: things block is %d bytes, expected %d "
                            "(count=%d)" % (tag, len(sc.things),
                                            40 * sc.count, sc.count))
        if len(sc.code) != sc.m:
            problems.append("%s: m=%d but code is %d bytes"
                            % (tag, sc.m, len(sc.code)))
        if sc.m > MAX_CODE:
            problems.append("%s: code %d bytes exceeds MAX_BLOCK 0x%x"
                            % (tag, sc.m, MAX_CODE))
        if sc.count > MAX_THINGS:
            problems.append("%s: things count %d exceeds 0x%x"
                            % (tag, sc.count, MAX_THINGS))
        if sc.n > MAX_HANDLES:
            problems.append("%s: handle count %d exceeds 0x%x"
                            % (tag, sc.n, MAX_HANDLES))
        if sc.flag:
            if len(sc.tables) != 9 * 0x400:
                problems.append("%s: flag=%d but tables are %d bytes, expected "
                                "0x%x" % (tag, sc.flag, len(sc.tables), 9 * 0x400))
        elif sc.tables:
            problems.append("%s: flag=0 but %d table bytes present"
                            % (tag, len(sc.tables)))
        try:
            columns = script_columns(sc)
            starts = list(columns[2])
        except WriteError as exc:
            problems.append("%s: %s" % (tag, exc))
            continue
        if starts != sorted(starts):
            problems.append("%s: sub-script offsets are not ascending" % tag)
        if starts and starts[0] != 0:
            problems.append("%s: no main sub-script at offset 0 (first table "
                            "entry is 0x%x)" % (tag, starts[0]))
        if len(set(starts)) != len(starts):
            problems.append("%s: duplicate sub-script offsets in column 2" % tag)
        try:
            spans = block_spans_of(sc, tag)
        except WriteError as exc:
            problems.append(str(exc))
            continue
        if not spans or spans[0][0] != 0:
            problems.append("%s: first sub-script does not start at 0" % tag)
        elif spans[-1][1] != len(sc.code):
            problems.append("%s: last sub-script ends at 0x%x, code is 0x%x "
                            "bytes" % (tag, spans[-1][1], len(sc.code)))
        else:
            for a, b in zip(spans, spans[1:]):
                if a[1] != b[0]:
                    problems.append("%s: gap/overlap between sub-scripts at "
                                    "0x%x..0x%x" % (tag, a[1], b[0]))
                    break
    return problems


def encoding_drift(sbt) -> list:
    """Records whose structural encoder disagrees with the bytes on disk.

    This is **informational**: the writer always emits ``Record.raw`` (see
    :meth:`gm.sbt.Sbt.to_bytes`), so such bytes are preserved verbatim anyway.
    A non-empty list only means the structured model in ``gm/sbt.py`` does not
    cover every byte of that record (e.g. an unmodelled padding), which is
    exactly the case where raw preservation matters.
    """
    out = []
    for i, r in enumerate(sbt.records):
        if r.type == END:
            continue
        try:
            enc = sbtmod.encode_payload(r.type, r.payload)
        except (ValueError, struct.error) as exc:
            out.append("record #%d %s: structural encoder failed: %s"
                       % (i, r.name, exc))
            continue
        if enc != r.raw:
            out.append("record #%d %s: structured form is %d bytes, raw payload "
                       "is %d (kept verbatim from the file)"
                       % (i, r.name, len(enc), len(r.raw)))
    return out


# ---------------------------------------------------------------------------
# replacement


def _script_record(sbt):
    recs = [r for r in sbt.records if r.type == SCRIPT_TYPE]
    if not recs:
        raise WriteError("the level has no SCRIPT record (type 11)")
    if len(recs) > 1:
        raise WriteError("the level has %d SCRIPT records; only one is "
                         "supported" % len(recs))
    return recs[0]


def new_script(old, code: bytes, handles: bytes):
    """Copy of a ``Script`` payload with new code/handles and ``m`` fixed."""
    return sbtmod.Script(old.count, old.n, len(code), old.flag,
                         old.things, handles, code, old.tables)


def replace_block(sbt, offset: int, new_block: bytes) -> dict:
    """Return ``(patched_copy, report)`` for replacing one sub-script.

    The input ``sbt`` is not modified (a deep copy is patched).  Raises
    :class:`WriteError` with a user-facing message on every inconsistency.
    """
    validate_block(new_block, "new block")
    rec = _script_record(sbt)
    sc = rec.payload
    columns = script_columns(sc)
    starts = list(columns[2])
    known = sorted(set([0] + starts))
    if offset not in known:
        raise WriteError("block 0x%x is not a sub-script entry (main block is 0, "
                         "table has %d entries; nearest: %s)"
                         % (offset, len(starts),
                            ", ".join("0x%x" % x for x in
                                      sorted(known, key=lambda v: abs(v - offset))[:4])))
    spans = dict(block_spans_of(sc))
    if offset not in spans:
        raise WriteError("block 0x%x has no END terminator in the code" % offset)
    old_start, old_end = offset, spans[offset]
    old_len = old_end - old_start
    new_len = len(new_block)
    delta = new_len - old_len
    new_code = sc.code[:old_start] + new_block + sc.code[old_end:]
    if len(new_code) > MAX_CODE:
        raise WriteError("replacement would grow the code buffer to %d bytes, "
                         "over the engine limit MAX_BLOCK=0x%x (%d)"
                         % (len(new_code), MAX_CODE, MAX_CODE))
    if delta:
        shifted = []
        for i, value in enumerate(starts):
            if old_start < value < old_end:
                raise WriteError("sub-script 0x%x lies inside the replaced span "
                                 "0x%x..0x%x; overlapping blocks are not "
                                 "supported" % (value, old_start, old_end))
            if value >= old_end:
                starts[i] = value + delta
                shifted.append(i)
    else:
        shifted = []
    if starts != sorted(starts):
        raise WriteError("internal: shifted sub-script offsets are not ascending")
    columns = list(columns)
    columns[2] = starts
    handles = encode_columns(columns)
    patched = new_script(sc, new_code, handles)

    out = copy.deepcopy(sbt)
    out_rec = out.records[next(i for i, r in enumerate(sbt.records)
                               if r is rec)]
    before = {"offset": out_rec.offset, "size": out_rec.size}
    try:
        out.set_payload(out_rec, patched)
    except (ValueError, struct.error) as exc:
        raise WriteError("cannot re-encode SCRIPT: %s" % exc)
    off = out.header_size
    for r in out.records:
        r.offset = off
        off += r.size
    problems = structure_problems(out)
    if problems:
        raise WriteError("edited container is not self-consistent: %s"
                         % "; ".join(problems))
    return out, {
        "block": old_start,
        "old_end": old_end,
        "old_size": old_len,
        "new_size": new_len,
        "delta": delta,
        "shifted_offsets": len(shifted),
        "record_before": before,
        "record_after": {"offset": out_rec.offset, "size": out_rec.size},
        "script": {"count": patched.count, "n": patched.n, "m": patched.m,
                   "flag": patched.flag,
                   "code_len": len(new_code),
                   "code_sha256": sha256(new_code)},
    }


# ---------------------------------------------------------------------------
# writing + verifying a replacement


def _block_at(script, offset):
    spans = dict(block_spans_of(script))
    if offset not in spans:
        raise WriteError("block 0x%x not found in the written file" % offset)
    return script.code[offset:spans[offset]]


def write_replacement(src_path, offset, new_block, out_path,
                      expect: dict | None = None) -> dict:
    """Replace one block, write the file, re-read it and verify everything."""
    data, sbt = load_container(src_path)
    out_sbt, rep = replace_block(sbt, offset, new_block)
    out_data = out_sbt.to_bytes()
    write_bytes(out_path, out_data)
    # -- oracle re-read
    back_data, back = load_container(out_path)
    problems = structure_problems(back, back_data)
    warnings = encoding_drift(back)
    if back_data != out_data:
        problems.append("written file differs from the bytes we produced")
    others_ok = len(back.records) == len(sbt.records)
    if not others_ok:
        problems.append("record count changed: %d -> %d"
                        % (len(sbt.records), len(back.records)))
    else:
        src_script = _script_record(sbt)
        dst_script = _script_record(back)
        for i, (a, b) in enumerate(zip(sbt.records, back.records)):
            if a is src_script:
                if b is not dst_script:
                    problems.append("SCRIPT record changed position")
                continue
            if a.type != b.type:
                problems.append("record #%d type changed %d -> %d"
                                % (i, a.type, b.type))
                others_ok = False
            elif a.raw != b.raw:
                problems.append("record #%d %s bytes changed (%d -> %d bytes)"
                                % (i, a.name, len(a.raw), len(b.raw)))
                others_ok = False
        old_sc = src_script.payload
        new_sc = dst_script.payload
        for field in ("count", "n", "flag", "things", "tables"):
            if getattr(old_sc, field) != getattr(new_sc, field):
                problems.append("SCRIPT.%s changed" % field)
        if new_sc.m != len(new_sc.code):
            problems.append("written SCRIPT.m=%d != code length %d"
                            % (new_sc.m, len(new_sc.code)))
        want_code = old_sc.code[:offset] + new_block + old_sc.code[offset + rep["old_size"]:]
        if new_sc.code != want_code:
            problems.append("written code differs from the expected splice")
        if _block_at(new_sc, offset) != new_block:
            problems.append("written block 0x%x does not equal the assembled "
                            "bytes" % offset)
        old_cols = script_columns(old_sc)
        new_cols = script_columns(new_sc)
        if old_cols[0] != new_cols[0] or old_cols[1] != new_cols[1]:
            problems.append("handle columns 0/1 changed")
        want_col2 = [(v + rep["delta"] if v >= rep["old_end"] else v)
                     for v in old_cols[2]]
        if new_cols[2] != want_col2:
            problems.append("handle column 2 (sub-script offsets) was not "
                            "shifted as expected")
    rep.update({
        "input": src_path,
        "output": out_path,
        "sha256": sha256(out_data),
        "bytes_in": len(data),
        "bytes_out": len(out_data),
        "size_delta": len(out_data) - len(data),
        "records": len(sbt.records),
        "other_records_identical": others_ok,
        "warnings": warnings,
        "target_block_equals_assembled": _block_at(
            _script_record(back).payload, offset) == new_block,
        "problems": problems,
    })
    if expect is not None:
        diffs = compare_plan(expect, plan_digest(back))
        rep["expect_diffs"] = diffs
        if diffs:
            rep["problems"].extend(diffs)
    rep["ok"] = not rep["problems"]
    return rep


# ---------------------------------------------------------------------------
# --check


def compare_plan(expect: dict, actual: dict) -> list:
    """Diff an expected plan (from ``--json``) against an actual digest."""
    diffs = []
    e = expect.get("container", {})
    a = actual.get("container", {})
    if e.get("version") != a.get("version"):
        diffs.append("version: expected %r, got %r"
                     % (e.get("version"), a.get("version")))
    if e.get("header_sha256") != a.get("header_sha256"):
        diffs.append("header sha256 differs")
    er, ar = e.get("records"), a.get("records")
    if not isinstance(er, list):
        diffs.append("expected plan has no container.records")
        er = []
    if len(er) != len(ar):
        diffs.append("record count: expected %d, got %d" % (len(er), len(ar)))
    for i, (x, y) in enumerate(zip(er, ar)):
        if x.get("type") != y.get("type"):
            diffs.append("record #%d type: expected %r, got %r"
                         % (i, x.get("type"), y.get("type")))
        if x.get("size") != y.get("size"):
            diffs.append("record #%d size: expected %r, got %r"
                         % (i, x.get("size"), y.get("size")))
        if x.get("sha256") != y.get("sha256"):
            diffs.append("record #%d sha256 differs" % i)
    es, as_ = expect.get("script"), actual.get("script")
    if es is not None:
        if as_ is None:
            diffs.append("expected a SCRIPT record, none found")
        else:
            for key in ("count", "n", "m", "flag", "things_len", "handles_len",
                        "tables_len", "code_len", "code_sha256"):
                if es.get(key) != as_.get(key):
                    diffs.append("SCRIPT.%s: expected %r, got %r"
                                 % (key, es.get(key), as_.get(key)))
            eb, ab = es.get("blocks", []), as_.get("blocks", [])
            if len(eb) != len(ab):
                diffs.append("SCRIPT blocks: expected %d, got %d"
                             % (len(eb), len(ab)))
            for i, (x, y) in enumerate(zip(eb, ab)):
                if x != y:
                    diffs.append("SCRIPT block #%d differs (0x%x)"
                                 % (i, y.get("offset", 0)))
                    break
    return diffs


def check_file(path, expect=None) -> dict:
    data, sbt = load_container(path)
    problems = structure_problems(sbt, data)
    warnings = encoding_drift(sbt)
    rep = {
        "tool": "sbtwrite", "mode": "check", "file": path,
        "sha256": sha256(data), "bytes": len(data),
        "records": len(sbt.records), "warnings": warnings,
    }
    plan = plan_digest(sbt) if not problems else None
    if plan is not None:
        rep.update(plan)
    if expect is not None:
        if plan is None:
            problems.append("cannot compare: the file does not parse cleanly")
        else:
            diffs = compare_plan(expect, plan)
            rep["expect_diffs"] = diffs
            problems.extend(diffs)
    rep["problems"] = problems
    rep["ok"] = not problems
    return rep


# ---------------------------------------------------------------------------
# --roundtrip


def roundtrip_corpus(game=DEFAULT_GAME, out_dir=None, verbose=False) -> dict:
    out_dir = out_dir or os.path.join(DEFAULT_OUT_DIR, "roundtrip")
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if not files:
        raise WriteError("no .sbt files in %s (use --game DIR)" % game)
    started = time.time()
    rows = []
    identical = mismatch = errors = 0
    structurally_ok = 0
    total_bytes = 0
    for path in files:
        row = {"file": os.path.basename(path)}
        try:
            data = read_bytes(path)
            sbt = parse_container(data, path)
            out = sbt.to_bytes()
            out_path = os.path.join(out_dir, os.path.basename(path))
            write_bytes(out_path, out)
            back = read_bytes(out_path)
            problems = structure_problems(sbt, data)
            drift = encoding_drift(sbt)
            row.update({
                "bytes": len(data),
                "sha256_in": sha256(data),
                "sha256_out": sha256(back),
                "written": out_path,
                "records": len(sbt.records),
                "problems": problems,
                "warnings": drift,
            })
            if back == data and out == data:
                identical += 1
                row["identical"] = True
            else:
                mismatch += 1
                row["identical"] = False
                first = next((i for i in range(min(len(back), len(data)))
                              if back[i] != data[i]), None)
                row["first_diff"] = first
            if problems:
                row["ok"] = False
            else:
                structurally_ok += 1
                row["ok"] = True
            total_bytes += len(data)
        except WriteError as exc:
            errors += 1
            row.update({"ok": False, "error": str(exc)})
        rows.append(row)
        if verbose:
            state = "identical" if row.get("identical") else (
                "ERROR" if "error" in row else "MISMATCH")
            print("  %-16s %8d bytes  %s" % (row["file"],
                                             row.get("bytes", 0), state))
    rep = {
        "tool": "sbtwrite", "mode": "roundtrip", "game": game,
        "out_dir": out_dir,
        "files": len(files), "bytes": total_bytes,
        "identical": identical, "mismatch": mismatch, "errors": errors,
        "structurally_ok": structurally_ok,
        "encoding_drift": sum(len(r.get("warnings", [])) for r in rows),
        "seconds": round(time.time() - started, 2),
        "ok": identical == len(files) and structurally_ok == len(files),
        "results": rows,
    }
    return rep


def print_roundtrip(rep) -> None:
    print("roundtrip: %d files, %d bytes, %.2fs"
          % (rep["files"], rep["bytes"], rep["seconds"]))
    print("  identical: %d/%d (%.2f%%), %d mismatch, %d error"
          % (rep["identical"], rep["files"],
             100.0 * rep["identical"] / max(1, rep["files"]),
             rep["mismatch"], rep["errors"]))
    print("  structural checks: %d/%d (raw round-trip, sizes, SCRIPT m and "
          "sub-script coverage)" % (rep["structurally_ok"], rep["files"]))
    print("  structural-encoder drift: %d record(s)%s"
          % (rep["encoding_drift"],
             "" if not rep["encoding_drift"]
             else " (raw bytes kept verbatim)"))


# ---------------------------------------------------------------------------
# real-level replacement verification


def _find_haze_block(script):
    """First sub-script containing ``SET_CAMERA_HAZE`` (fixed 17-byte form)."""
    for start, end in block_spans_of(script):
        block = script.code[start:end]
        try:
            insns, _ = sbtvm.decode(block)
        except (ValueError, struct.error):
            continue
        for ins in _walk(insns):
            if ins.name == "SET_CAMERA_HAZE":
                return start, block
    return None, None


def _walk(insns):
    for ins in insns:
        yield ins
        if isinstance(ins.body, list):
            yield from _walk(ins.body)


def _haze_edit(text):
    """Change the first ``SET_CAMERA_HAZE`` literal; same byte length."""
    lines = text.split("\n")
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith("SET_CAMERA_HAZE "):
            continue
        tokens = stripped.split()
        if len(tokens) != 3:
            raise WriteError("unexpected SET_CAMERA_HAZE text %r" % stripped)
        try:
            first = float(tokens[1])
        except ValueError:
            continue
        indent = line[:len(line) - len(line.lstrip())]
        lines[i] = "%sSET_CAMERA_HAZE %s %s" % (indent, repr(first + 0.5),
                                                tokens[2])
        return "\n".join(lines), i
    raise WriteError("no editable SET_CAMERA_HAZE line in the rendered text")


def verify_replace(game=DEFAULT_GAME, levels=("level0", "dm1", "level24"),
                   out_dir=None) -> dict:
    """Replace one block on real levels; prove the rest is untouched."""
    out_dir = out_dir or os.path.join(DEFAULT_OUT_DIR, "verify")
    rows = []
    for level in levels:
        path = sbtasm.level_path(level, game)
        if not path:
            raise WriteError("level %r not found in %s" % (level, game))
        _, sbt = load_container(path)
        script = _script_record(sbt).payload
        offset, block = _find_haze_block(script)
        if offset is None:
            raise WriteError("%s: no sub-script with SET_CAMERA_HAZE" % level)
        text = sbtasm.render_exact(block)
        rebuilt = sbtasm.assemble(text)
        row = {"level": level, "file": os.path.basename(path), "block": offset,
               "old_size": len(block),
               "render_roundtrip": rebuilt == block}
        variants = []
        # (a) same length: one double literal changes
        edited_text, _line = _haze_edit(text)
        same = sbtasm.assemble(edited_text)
        if len(same) != len(block):
            raise WriteError("%s: same-length edit changed the size (%d -> %d)"
                             % (level, len(block), len(same)))
        diff = sum(1 for a, b in zip(block, same) if a != b)
        if not 1 <= diff <= 8:
            raise WriteError("%s: same-length edit changed %d bytes, expected "
                             "1..8 (one double)" % (level, diff))
        # (b) one extra valid instruction before END: +1 byte
        end_line = "\n---"
        if not text.endswith(end_line):
            raise WriteError("%s: rendered block does not end with '---'" % level)
        longer_text = text[:-len(end_line)] + "\nEXIT_EVENT" + end_line
        longer = sbtasm.assemble(longer_text)
        if longer != block[:-1] + b"\x01\xff":
            raise WriteError("%s: longer edit is not old block + EXIT_EVENT"
                             % level)
        for variant, new_block in (("same-length", same), ("longer", longer)):
            out_path = os.path.join(
                out_dir, "%s_%s.sbt" % (level, variant.replace("-", "_")))
            rep = write_replacement(path, offset, new_block, out_path)
            rep["variant"] = variant
            rep["level"] = level
            variants.append(rep)
            if not rep["ok"]:
                rows.append({"level": level, "variant": variant,
                             "ok": False, "problems": rep["problems"]})
        rows.append({
            "level": level, "file": os.path.basename(path), "block": offset,
            "old_size": len(block), "render_roundtrip": rebuilt == block,
            "variants": variants,
            "block_sha256": sha256(block),
            "ok": all(v["ok"] for v in variants),
        })
    good = [r for r in rows if r.get("ok")]
    variants = sum(len(r.get("variants", [])) for r in rows)
    return {
        "tool": "sbtwrite", "mode": "verify-replace", "game": game,
        "out_dir": out_dir, "levels": len(levels),
        "variants": variants,
        "identical_others": sum(1 for r in rows
                                for v in r.get("variants", [])
                                if v.get("other_records_identical")),
        "ok": len(good) == len(rows) and bool(rows),
        "results": rows,
    }


def print_verify(rep) -> None:
    print("verify-replace: %d levels, %d variants"
          % (rep["levels"], rep["variants"]))
    for row in rep["results"]:
        if "variants" not in row:
            print("  %-8s %-12s FAILED: %s"
                  % (row["level"], row["variant"],
                     "; ".join(row.get("problems", []))))
            continue
        print("  %-8s block 0x%-6x %d bytes; render->asm exact: %s"
              % (row["level"], row["block"], row["old_size"],
                 "yes" if row["render_roundtrip"] else "NO"))
        for v in row["variants"]:
            print("    %-12s %d -> %d bytes (%+d), shifted %d offsets, "
                  "others identical: %s, block==asm: %s, check: %s"
                  % (v["variant"], v["old_size"], v["new_size"], v["delta"],
                     v["shifted_offsets"],
                     "yes" if v["other_records_identical"] else "NO",
                     "yes" if v["target_block_equals_assembled"] else "NO",
                     "ok" if not v["problems"] else "FAILED"))


# ---------------------------------------------------------------------------
# synthetic self-test (no game files)


def _synth_container(script: bytes, extras=(), version: int = 0) -> bytes:
    header = (sbtmod.MAGIC + b"\0" * (80 - len(sbtmod.MAGIC))
              + struct.pack("<I", version))
    parts = [header]
    for type_, payload in extras:
        parts.append(bytes((type_,)) + payload)
    parts.append(bytes((SCRIPT_TYPE,)) + script)
    parts.append(bytes((END,)))
    return b"".join(parts)


def _synth_script(code: bytes, handles) -> bytes:
    columns = [tuple(c) for c in handles]
    n = len(columns[0]) if columns else 0
    return (struct.pack("<I", 0) + struct.pack("<I", n)
            + encode_columns(columns) + struct.pack("<I", len(code))
            + code + b"\0")


def _synth_extras():
    switch = struct.pack("<I", 0)
    sprite = bytes(range(sbtmod.SPRITE_SIZE))
    handles = struct.pack("<I", 2) + struct.pack("<2I", 1, 2) \
        + struct.pack("<2I", 3, 4)
    sob = (struct.pack("<I", 1) + b"crate".ljust(12, b"\0")
           + struct.pack("<II", 7, 0xDEADBEEF))
    return ((1, switch), (6, struct.pack("<I", 1) + sprite),
            (17, handles), (20, sob))


def _selftest() -> bool:
    results = []

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

    def expect_error(fn, needle, label):
        try:
            fn()
        except WriteError as exc:
            if needle not in str(exc):
                raise AssertionError("%s: message %r lacks %r"
                                     % (label, str(exc), needle))
            return str(exc)
        raise AssertionError("%s: no WriteError raised" % label)

    # two blocks: "EXIT_EVENT; END" (2 bytes) and "END" (1 byte)
    block_a = b"\x01\xff"
    block_b = b"\xff"
    code = block_a + block_b
    handles = ((0, 0), (511, 511), (0, 2))   # column 2: block starts 0 and 2
    script = _synth_script(code, handles)
    base = _synth_container(script, _synth_extras())

    def t_roundtrip():
        sbt = sbtmod.parse(base)
        assert sbt.to_bytes() == base, "to_bytes differs from synthetic input"
        assert len(sbt.records) == 6, len(sbt.records)     # 4 extras + SCRIPT + END
        assert structure_problems(sbt, base) == [], \
            structure_problems(sbt, base)
        return "%d records, %d bytes" % (len(sbt.records), len(base))

    def t_header_raw():
        tweaked = bytearray(base)
        tweaked[0x20:0x24] = b"\xa5\x5a\xa5\x5a"  # header padding after the magic
        sbt = sbtmod.parse(bytes(tweaked))
        assert sbt.to_bytes() == bytes(tweaked), "header padding was not kept"
        return "header padding preserved"

    def t_same_length():
        sbt = sbtmod.parse(base)
        out, rep = replace_block(sbt, 0, b"\x07\x00\x00\x00\x00\xff")
        # EXIT_EVENT -> SLEEP 0 (5+1 bytes), block 0 grows 2 -> 6
        assert rep["delta"] == 4, rep["delta"]
        assert rep["shifted_offsets"] == 1, rep["shifted_offsets"]
        back = sbtmod.parse(out.to_bytes())
        sc = _script_record(back).payload
        assert sc.code[:6] == b"\x07\x00\x00\x00\x00\xff"
        assert sc.code[6:] == block_b
        cols = script_columns(sc)
        assert cols[2] == [0, 6], cols[2]
        others = [r for i, r in enumerate(back.records) if i != 4]
        orig = [r for i, r in enumerate(sbt.records) if i != 4]
        assert [r.raw for r in others] == [r.raw for r in orig]
        return "delta +4, one offset shifted, 5 other records identical"

    def t_longer_write(tmp):
        out_path = os.path.join(tmp, "longer.sbt")
        rep = write_replacement(_write(base, tmp, "base.sbt"), 2,
                                b"\x01\xff", out_path)
        assert rep["ok"], rep["problems"]
        assert rep["delta"] == 1
        assert rep["other_records_identical"]
        assert rep["target_block_equals_assembled"]
        assert rep["bytes_out"] == len(base) + 1
        chk = check_file(out_path)
        assert chk["ok"], chk["problems"]
        return "block 2 1 -> 2 bytes, file +1, check ok"

    def t_shorter_write(tmp):
        longer_code = b"\x07\x01\x00\x00\x00\xff" + block_b     # 6 + 1 bytes
        src = _write(_synth_container(
            _synth_script(longer_code, ((0, 0), (511, 511), (0, 6))),
            _synth_extras()),
            tmp, "shorter-in.sbt")
        rep = write_replacement(src, 0, b"\xff",
                                os.path.join(tmp, "shorter.sbt"))
        assert rep["ok"], rep["problems"]
        assert rep["delta"] == -5, rep["delta"]
        sc = _script_record(sbtmod.parse(read_bytes(rep["output"]))).payload
        assert sc.code == b"\xff\xff", sc.code
        assert script_columns(sc)[2] == [0, 1], script_columns(sc)[2]
        return "6-byte block -> END (1 byte), file -5, offset 6 -> 1"

    def t_no_script():
        header = (sbtmod.MAGIC + b"\0" * (80 - len(sbtmod.MAGIC))
                  + struct.pack("<I", 0))
        data = header + b"\x01\x00\x00\x00\x00\xff"
        sbt = sbtmod.parse(data)
        return expect_error(lambda: replace_block(sbt, 0, b"\xff"),
                            "no SCRIPT record", "no-script")

    def t_bad_offset():
        sbt = sbtmod.parse(base)
        return expect_error(lambda: replace_block(sbt, 3, b"\xff"),
                            "not a sub-script entry", "offset")

    def t_empty_block():
        sbt = sbtmod.parse(base)
        return expect_error(lambda: replace_block(sbt, 0, b""),
                            "is empty", "empty")

    def t_no_end():
        sbt = sbtmod.parse(base)
        return expect_error(lambda: replace_block(sbt, 0, b"\x01"),
                            "does not end with END", "no-end")

    def t_trailing():
        sbt = sbtmod.parse(base)
        return expect_error(lambda: replace_block(sbt, 0, b"\xff\xff"),
                            "END (0xFF) byte(s) inside", "trailing")

    def t_bad_opcode():
        sbt = sbtmod.parse(base)
        return expect_error(lambda: replace_block(sbt, 0, b"\xfe\xff"),
                            "not valid SCRIPT bytecode", "opcode")

    def t_too_big():
        sbt = sbtmod.parse(base)
        big = b"\x07\x00\x00\x00\x00" * ((MAX_CODE // 5) + 1) + b"\xff"
        return expect_error(lambda: replace_block(sbt, 0, big),
                            "MAX_BLOCK", "limit")

    def t_bad_magic():
        data = bytearray(base)
        data[0:4] = b"NOPE"
        return expect_error(lambda: parse_container(bytes(data), "test.sbt"),
                            "bad .sbt magic", "magic")

    def t_raw_preserved():
        data = _synth_container(script, ((8, bytes(0x5800)),))
        sbt = sbtmod.parse(data)
        rec = sbt.records[0]
        rec.raw = bytes([0x21]) + rec.raw[1:]     # opaque slot byte
        out = sbt.to_bytes()
        assert out[84 + 1] == 0x21, "mutated raw byte was not emitted"
        assert len(out) == len(data), (len(out), len(data))
        drift = encoding_drift(sbt)
        assert drift and "kept verbatim" in drift[0], drift
        sbtmod.parse(out)                          # still a valid container
        return "opaque byte kept; 1 encoder-drift warning"

    def t_bad_m():
        sbt = sbtmod.parse(base)
        sbt = copy.deepcopy(sbt)
        _script_record(sbt).payload.m += 1
        problems = structure_problems(sbt)
        assert any("m=" in p for p in problems), problems
        return problems[0]

    def t_check_tamper(tmp):
        path = _write(base, tmp, "tamper.sbt")
        assert check_file(path)["ok"], "clean file failed check"
        data = bytearray(read_bytes(path))
        # locate the SCRIPT payload: header, then the preceding record sizes
        p = 84
        for r in sbtmod.parse(base).records:
            if r.type == SCRIPT_TYPE:
                break
            p += r.size
        assert data[p] == SCRIPT_TYPE, data[p]
        code_off = p + 1 + 4 + 4 + 12 + 4               # payload+count+n+cols+m
        data[code_off] = 0xFE                           # unknown opcode in block 0
        bad_path = os.path.join(tmp, "tamper-bad.sbt")
        write_bytes(bad_path, bytes(data))
        rep = check_file(bad_path)
        assert not rep["ok"], "tampered file passed check"
        return "clean ok, tampered code reported: %s" % rep["problems"][0]

    def t_expect(tmp):
        path = _write(base, tmp, "plan.sbt")
        plan = plan_digest(sbtmod.parse(base))
        assert check_file(path, expect=plan)["ok"], "plan comparison failed"
        plan2 = copy.deepcopy(plan)
        plan2["script"]["code_sha256"] = "0" * 64
        rep = check_file(path, expect=plan2)
        assert not rep["ok"], "plan mismatch passed"
        assert any("code_sha256" in p for p in rep["problems"]), rep["problems"]
        return "plan match ok; code hash mismatch detected"

    def t_write_verify(tmp):
        src = _write(base, tmp, "v-in.sbt")
        out = os.path.join(tmp, "v-out.sbt")
        rep = write_replacement(src, 2, b"\x01\xff", out)
        assert rep["ok"], rep["problems"]
        assert rep["sha256"] == sha256(read_bytes(out))
        return "%d bytes in, %d out" % (rep["bytes_in"], rep["bytes_out"])

    with tempfile.TemporaryDirectory(prefix="sbtwrite-selftest-") as tmp:
        case("synthetic round-trip (parse -> to_bytes)", t_roundtrip)
        case("header kept verbatim (padding bytes)", t_header_raw)
        case("replace block: sizes, shifted offsets, others' raw", t_same_length)
        case("write + oracle re-read: longer block", lambda: t_longer_write(tmp))
        case("write + oracle re-read: shorter block", lambda: t_shorter_write(tmp))
        case("error: no SCRIPT record", t_no_script)
        case("error: offset is not a sub-script", t_bad_offset)
        case("error: empty block", t_empty_block)
        case("error: block does not end with END", t_no_end)
        case("error: trailing bytes after END", t_trailing)
        case("error: unknown opcode", t_bad_opcode)
        case("error: code over MAX_BLOCK", t_too_big)
        case("error: bad magic", t_bad_magic)
        case("opaque record bytes preserved (encoder drift)", t_raw_preserved)
        case("structure: m != code length", t_bad_m)
        case("--check: clean ok, tampered code fails", lambda: t_check_tamper(tmp))
        case("--check --expect plan match/mismatch", lambda: t_expect(tmp))
        case("write --out: file equals reported sha256", lambda: t_write_verify(tmp))

    failed = [r for r in results if not r[1]]
    for name, ok, detail in results:
        print("  %-52s %s%s" % (name, "ok" if ok else "FAIL",
                                ("  (" + detail + ")") if detail and not ok
                                else ""))
    print("%d/%d self-tests passed" % (len(results) - len(failed), len(results)))
    return not failed


def _write(data: bytes, tmp: str, name: str) -> str:
    path = os.path.join(tmp, name)
    write_bytes(path, data)
    return path


# ---------------------------------------------------------------------------
# CLI


def _load_asm(args) -> bytes:
    if args.asm and args.text is not None:
        raise WriteError("use either --asm FILE or --text TEXT, not both")
    if args.asm:
        text = read_bytes(args.asm).decode("latin1")
        origin = args.asm
    elif args.text is not None:
        if args.text.startswith("@"):
            origin = args.text[1:]
            text = read_bytes(origin).decode("latin1")
        else:
            text = args.text
            origin = "<text>"
    else:
        raise WriteError("a replacement needs --asm FILE or --text TEXT")
    try:
        block = sbtasm.assemble(text)
    except sbtasm.AsmError as exc:
        raise WriteError("assembling %s: %s" % (origin, exc))
    except (ValueError, struct.error) as exc:
        raise WriteError("assembling %s: %s" % (origin, exc))
    validate_block(block, "assembled block from %s" % origin)
    return block


def _parse_offset(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError:
        try:
            value = int(text, 16)
        except ValueError:
            raise WriteError("--block %r is not a number (use 0x.. or decimal)"
                             % text)
    if value < 0:
        raise WriteError("--block must not be negative")
    return value


def _cmd_write(args) -> int:
    level = args.script
    path = sbtasm.level_path(level, args.game or DEFAULT_GAME)
    if not path:
        raise WriteError("level %r not found (tried as a path and in %s)"
                         % (level, args.game or DEFAULT_GAME))
    if not args.out:
        raise WriteError("--out OUT.sbt is required when writing")
    offset = _parse_offset(args.block)
    block = _load_asm(args)
    expect = None
    if args.expect:
        expect = _read_json(args.expect, "--expect")
    rep = write_replacement(path, offset, block, args.out, expect=expect)
    rep["mode"] = "write"
    rep["tool"] = "sbtwrite"
    # the report doubles as a --check --expect plan
    back = sbtmod.parse(read_bytes(args.out))
    plan = plan_digest(back)
    rep["container"] = plan["container"]
    if "script" in plan:
        rep["script"] = plan["script"]
    if args.plan:
        write_bytes(args.plan,
                    (json.dumps(plan, indent=1) + "\n").encode())
        rep["plan"] = args.plan
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        if rep["ok"]:
            print("wrote %s (%d bytes, sha256 %s)"
                  % (rep["output"], rep["bytes_out"], rep["sha256"][:16]))
            print("  block 0x%x: %d -> %d bytes (%+d), shifted %d sub-script "
                  "offset(s)"
                  % (rep["block"], rep["old_size"], rep["new_size"],
                     rep["delta"], rep["shifted_offsets"]))
            print("  other records byte-identical: %s; target block == "
                  "assembled: %s; check: %s"
                  % ("yes" if rep["other_records_identical"] else "NO",
                     "yes" if rep["target_block_equals_assembled"] else "NO",
                     "ok" if not rep["problems"] else "FAILED"))
            if rep.get("plan"):
                print("  plan: %s" % rep["plan"])
        else:
            print("write FAILED: %s" % rep["output"])
            for p in rep["problems"]:
                print("  - %s" % p)
    return 0 if rep["ok"] else 1


def _read_json(path, what):
    raw = read_bytes(path)
    try:
        return json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise WriteError("%s: %s is not JSON: %s" % (what, path, exc))


def _cmd_check(args) -> int:
    expect = _read_json(args.expect, "--expect") if args.expect else None
    rep = check_file(args.check, expect=expect)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        sc = rep.get("script")
        extra = ""
        if sc:
            extra = ", %d sub-scripts, code %d bytes" % (len(sc["blocks"]),
                                                         sc["code_len"])
        if rep["ok"]:
            print("check: %s ok (%d records%s, %d bytes)"
                  % (rep["file"], rep["records"], extra, rep["bytes"]))
            for w in rep.get("warnings", []):
                print("  warning: %s" % w)
        else:
            print("check: %s FAILED (%d records%s)"
                  % (rep["file"], rep["records"], extra))
            for p in rep["problems"]:
                print("  - %s" % p)
    return 0 if rep["ok"] else 1


def _cmd_roundtrip(args) -> int:
    rep = roundtrip_corpus(args.game or DEFAULT_GAME, args.out, args.verbose)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_roundtrip(rep)
    return 0 if rep["ok"] else 1


def _cmd_verify(args) -> int:
    rep = verify_replace(args.game or DEFAULT_GAME, out_dir=args.out)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_verify(rep)
    return 0 if rep["ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv:
        head = argv[0].lower()
        if head in ("self-test", "selftest", "--self-test", "--selftest"):
            argv = ["--self-test"] + argv[1:]
        elif head in ("roundtrip", "--roundtrip"):
            argv = ["--roundtrip"] + argv[1:]
        elif head in ("check", "--check") and len(argv) > 1:
            argv = ["--check", argv[1]] + argv[2:]
        elif head in ("verify-replace", "--verify-replace"):
            argv = ["--verify-replace"] + argv[1:]
    ap = argparse.ArgumentParser(
        description="Writer for the Gunmetal .sbt container (M11.4).")
    ap.add_argument("--roundtrip", action="store_true",
                    help="parse -> write -> compare every shipped .sbt")
    ap.add_argument("--script", metavar="LEVEL",
                    help="level .sbt path or stem (level0, dm1, ...)")
    ap.add_argument("--block", metavar="OFF",
                    help="sub-script offset to replace (0x.. or decimal)")
    ap.add_argument("--asm", metavar="FILE",
                    help="assembler text to insert (tools/editor/sbtasm.py)")
    ap.add_argument("--text", metavar="TEXT",
                    help="inline assembler text (@FILE reads a file)")
    ap.add_argument("--out", metavar="PATH",
                    help="output .sbt (write), directory (roundtrip/verify)")
    ap.add_argument("--check", metavar="FILE",
                    help="re-read FILE with the oracle and check the structure")
    ap.add_argument("--expect", metavar="FILE",
                    help="--check/--script: compare with this --json plan")
    ap.add_argument("--plan", metavar="FILE",
                    help="write the resulting structure digest (JSON) here")
    ap.add_argument("--verify-replace", action="store_true",
                    help="replace one block on level0/dm1/level24 and verify")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic tests (no game files)")
    ap.add_argument("--game", default=None,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if _selftest() else 1
    if args.roundtrip:
        return _cmd_roundtrip(args)
    if args.verify_replace:
        return _cmd_verify(args)
    if args.check:
        return _cmd_check(args)
    if args.script:
        return _cmd_write(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except WriteError as exc:
        print("sbtwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, struct.error, OSError) as exc:
        print("sbtwrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                           # noqa: BLE001
        print("sbtwrite: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
