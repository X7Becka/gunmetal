#!/usr/bin/env python3
"""Writer/editor for the Gunmetal ``.key`` key profiles (``KEYCFG/*.KEY``).

A ``.key`` file is a fixed 376-byte (0x178) record (``docs/FORMATS.md`` §8.2,
``docs/CONTROLS_CPP.md`` §1.1, loader ``0x4852e7``…``0x4853e6``)::

    +0x000  char name[16]              profile name, NUL padded
    +0x010  31 * { u32 code; u32 b }   the 31 actions (Move Forward … Linear Right)
    +0x108  14 * { u32 x;    u32 y }   opaque 112-byte tail (never interpreted)

The file physically stores **45** 8-byte pairs; the loader copies only the first
**31** (248 bytes) into its ``name[16] + 31*8`` record (0x108 bytes), and the
writer then dumps that record *plus* 112 bytes of its own uninitialised stack
(``docs/CONTROLS_CPP.md`` §1.1).  That is why the tail is identical in all three
shipped files and why this tool **preserves it verbatim and refuses to edit
it**: pair 31 is not action 31, it is garbage nobody has explained.

What is proven here (byte-exact round-trip, see ``docs/KEY_WRITE.md``):

* ``name[16] + 45*8`` is the whole file: ``parse -> to_bytes`` is the identity;
* slots 0..30 hold the action key code in ``a`` and a second u32 ``b`` that is
  **zero in every shipped pair** — so a "code + modifiers" reading of the pair
  is *not* confirmed, and is only reported, never written from guesswork;
* an edit of one slot touches exactly 8 bytes, an edit of the name only the
  first 16, and the tail is byte-identical before and after.

Usage::

    python3 tools/editor/keywrite.py --roundtrip              # game KEYCFG/
    python3 tools/editor/keywrite.py --roundtrip FILE.key
    python3 tools/editor/keywrite.py --audit                  # corpus numbers
    python3 tools/editor/keywrite.py --list KEYCFG/PERFECT.key [--json]
    python3 tools/editor/keywrite.py --check KEYCFG/PERFECT.key
    python3 tools/editor/keywrite.py --check OUT.key --against KEYCFG/PERFECT.key
    python3 tools/editor/keywrite.py --file KEYCFG/PERFECT.key \
            --set-slot 5 --bytes 1d00000000000000 --out /tmp/opencode/x.key
    python3 tools/editor/keywrite.py --file KEYCFG/PERFECT.key \
            --set-name NEWSET --out /tmp/opencode/x.key
    python3 tools/editor/keywrite.py --self-test [--json]

Exit codes: ``0`` success, ``1`` verification failed (round-trip/check/self-test
negative), ``2`` bad input or a refused edit, ``3`` internal error.

Refusals (code 2): not exactly 376 bytes; a name longer than 15 bytes or with a
non-NUL byte after the terminator; an empty target slot list; a slot in the
unknown 112-byte tail (31..44); ``--bytes`` that is not exactly 16 hex digits; a
name that is not latin1 / contains a NUL; ``--out`` inside the game installation,
its regression mirror or this repository, or ``--out`` equal to the input.
The game directory is read-only: this tool only ever reads it.

``--game DIR`` overrides the installation (default ``/mnt/d/games/Gunmetal 3dfx``).
The optional symbol column comes from the game's own 123-entry key-name table
(RVA 0x4A5B88), mirrored in ``engine/src/input/controls.cpp``; without that file
codes print as ``KEY_<n>`` and nothing else changes.  ``KEY_<n>`` is a display
fallback, not a validity check: this tool does **not** decide whether a code is
a legal scancode — the game's table is 123 entries and unknown codes are
preserved by the C++ reader.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass, field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import small as smallmod                     # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")
GAME = DEFAULT_GAME

KEY_SIZE = 376
NAME_SIZE = 16
SLOT_COUNT = 45          # pairs physically in the file
USED_SLOTS = 31          # pairs the loader copies into its 0x108-byte record
TAIL_OFFSET = NAME_SIZE + USED_SLOTS * 8   # 0x108 = 264
TAIL_BYTES = KEY_SIZE - TAIL_OFFSET        # 112
PAIR_BYTES = 8
MAX_NAME_BYTES = NAME_SIZE - 1             # 15 + NUL
SYMBOL_TABLE_CPP = os.path.join(ROOT, "engine", "src", "input", "controls.cpp")
SYMBOL_COUNT = 123                         # entries in the table at 0x4A5B88

#: actions of the 31-entry pointer table at 0x4BC558, as named by
#: ``engine/src/input/controls.cpp`` (``kActionNames``).
ACTION_NAMES = (
    "forward", "back", "turn_left", "turn_right", "look_center", "fire",
    "look_up", "look_down", "strafe", "strafe_left", "strafe_right",
    "throttle_up", "throttle_down", "next_weapon", "prev_weapon", "activate",
    "next_equipment", "prev_equipment", "toggle_equipment", "headlights",
    "force_field", "mine_detector", "targeting", "warp_device", "radar",
    "mine_layer", "combat_block", "auto_sentry", "decoy", "linear_left",
    "linear_right",
)
assert len(ACTION_NAMES) == USED_SLOTS

_SYMBOLS: list[str] | None = None


class WriteError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


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


def esc(data: bytes) -> str:
    """A printable, byte-faithful rendering of a latin1 name."""
    out = []
    for code in data:
        if 0x20 <= code < 0x7F:
            out.append(chr(code))
        elif code == 0x09:
            out.append("\\t")
        else:
            out.append("\\x%02x" % code)
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


def key_files(directory: str) -> list:
    """Every ``.key``/``.KEY`` under *directory* (recursive, case-insensitive)."""
    out = []
    for base, _dirs, names in os.walk(directory):
        for name in names:
            if os.path.splitext(name)[1].lower() == ".key":
                out.append(os.path.join(base, name))
    return sorted(out)


def slot_offset(slot: int) -> int:
    return NAME_SIZE + slot * PAIR_BYTES


# ---------------------------------------------------------------------------
# key-name table (display only)


def load_symbols(path: str = "") -> list:
    """The 123-entry key-name table, parsed from ``controls.cpp``."""
    global _SYMBOLS
    if _SYMBOLS is not None and not path:
        return _SYMBOLS
    source = path or SYMBOL_TABLE_CPP
    try:
        with open(source, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        if not path:
            _SYMBOLS = []
            return _SYMBOLS
        return []
    match = re.search(
        r"std::array<std::string_view,\s*%d>\s+kKeyNames\s*=\s*\{(.*?)\};"
        % SYMBOL_COUNT, text, re.S)
    table = re.findall(r'"([^"]*)"', match.group(1)) if match else []
    if not path:
        _SYMBOLS = table
    return table


def symbol_of(code: int, table: list) -> str:
    if code == 0:
        return "NONE"
    if code & 0x80000000:
        return "CORRUPT"
    if code < len(table):
        return table[code]
    return "KEY_%d" % code


# ---------------------------------------------------------------------------
# model (the oracle ``gm/small.py::parse_key`` reads the same layout)


@dataclass
class KeyFile:
    raw_name: bytes = b"\0" * NAME_SIZE
    pairs: list = field(default_factory=list)      # 45 * (u32 code, u32 second)

    @property
    def name(self) -> str:
        return self.raw_name.split(b"\0")[0].decode("latin1")

    @property
    def actions(self) -> list:
        return self.pairs[:USED_SLOTS]

    @property
    def tail(self) -> bytes:
        out = bytearray()
        for code, extra in self.pairs[USED_SLOTS:]:
            out += struct.pack("<II", code, extra)
        return bytes(out)

    @property
    def bound(self) -> int:
        return sum(1 for code, _extra in self.actions if code)

    def to_bytes(self) -> bytes:
        if len(self.pairs) != SLOT_COUNT:
            raise ValueError("internal: %d pairs, expected %d"
                             % (len(self.pairs), SLOT_COUNT))
        out = bytearray(self.raw_name[:NAME_SIZE].ljust(NAME_SIZE, b"\0"))
        for code, extra in self.pairs:
            out += struct.pack("<II", code, extra)
        if len(out) != KEY_SIZE:
            raise ValueError("internal: %d bytes, expected %d"
                             % (len(out), KEY_SIZE))
        return bytes(out)


def parse_key(data: bytes, where: str = "") -> KeyFile:
    """Read the documented fixed layout; refuses any other size."""
    if len(data) != KEY_SIZE:
        raise WriteError(
            "%s: expected %d bytes (name[16] + 45*8, docs/FORMATS.md §8.2), "
            "got %d" % (where or "input", KEY_SIZE, len(data)))
    pairs = [struct.unpack_from("<II", data, NAME_SIZE + PAIR_BYTES * i)
             for i in range(SLOT_COUNT)]
    return KeyFile(raw_name=data[:NAME_SIZE], pairs=pairs)


def inspect_name(raw: bytes) -> dict:
    """Canonical ``name[16]``: <=15 bytes, NUL, then only NUL padding."""
    problems = []
    warnings = []
    nul = raw.find(b"\0")
    if nul < 0:
        problems.append("name fills all 16 bytes with no NUL (longer than %d "
                        "bytes)" % MAX_NAME_BYTES)
        nul = NAME_SIZE
    elif any(raw[nul + 1:]):
        problems.append("non-NUL byte after the name's NUL at +%d: the padding "
                        "is not canonical" % nul)
    text = raw[:nul].decode("latin1")
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text):
        warnings.append("name contains control characters")
    if not text:
        warnings.append("name is empty (an unnamed profile)")
    return {"text": text, "nul_at": None if nul == NAME_SIZE else nul,
            "problems": problems, "warnings": warnings}


def require_canonical_name(key: KeyFile, where: str) -> dict:
    info = inspect_name(key.raw_name)
    if info["problems"]:
        raise WriteError("%s: %s" % (where, "; ".join(info["problems"])))
    return info


def encode_name(name: str) -> bytes:
    """Validate ``--set-name`` and return the canonical ``name[16]``."""
    try:
        data = name.encode("latin1")
    except UnicodeEncodeError:
        raise WriteError("--set-name %r is not latin1-encodable; the profile "
                         "name is a one-byte C string" % name)
    if b"\0" in data:
        raise WriteError("--set-name must not contain a NUL byte")
    if len(data) > MAX_NAME_BYTES:
        raise WriteError("--set-name is %d bytes; name[16] holds at most %d "
                         "bytes plus the NUL terminator" % (len(data),
                                                            MAX_NAME_BYTES))
    return data.ljust(NAME_SIZE, b"\0")


# ---------------------------------------------------------------------------
# --check / --list


def check_data(raw: bytes, where: str, table: list) -> dict:
    key = parse_key(raw, where)
    name = require_canonical_name(key, where)
    problems = []
    warnings = list(name["warnings"])
    unknown = []
    corrupt = []
    for slot, (code, extra) in enumerate(key.actions):
        if code & 0x80000000:
            corrupt.append(slot)
            problems.append("slot %d: code 0x%08x has the top bit set (the C++ "
                            "reader rejects it as corrupt)" % (slot, code))
        elif code >= SYMBOL_COUNT:
            unknown.append(slot)
            warnings.append("slot %d: code %d (0x%x) is outside the 123-entry "
                            "name table; preserved, not guessed"
                            % (slot, code, code))
        if extra:
            warnings.append("slot %d: second u32 is 0x%08x, not zero (the "
                            "layout is 'code + not-yet-understood value', not "
                            "proven modifiers); preserved" % (slot, extra))
    return {
        "file": where, "bytes": len(raw), "name": name["text"],
        "name_hex": key.raw_name.hex(), "bound": key.bound,
        "slots": USED_SLOTS, "tail_bytes": TAIL_BYTES,
        "problems": problems, "warnings": warnings,
        "unknown_slots": unknown, "corrupt_slots": corrupt,
        "ok": not problems, "key": key,
    }


def check_file(path: str, against: str | None = None) -> dict:
    raw = read_bytes(path)
    rep = check_data(raw, path, load_symbols())
    rep.pop("key")
    rep["sha256"] = sha256(raw)
    rep["against"] = None
    if against:
        other = read_bytes(against)
        other_rep = check_data(other, against, load_symbols())
        delta = diff_stats(other, raw)
        changed_slots = []
        name_bytes = 0
        tail_bytes = 0
        for i in range(PAIR_BYTES * SLOT_COUNT):
            off = NAME_SIZE + i
            if raw[off] != other[off]:
                if off < TAIL_OFFSET:
                    changed_slots.append((off - NAME_SIZE) // PAIR_BYTES)
                else:
                    tail_bytes += 1
        for i in range(NAME_SIZE):
            if raw[i] != other[i]:
                name_bytes += 1
        rep["against"] = {
            "file": against, "name": other_rep["name"],
            "changed": delta["changed"], "first_diff": delta["first_diff"],
            "name_bytes": name_bytes,
            "slots": sorted(set(changed_slots)), "tail_bytes": tail_bytes,
            "sha256": sha256(other),
        }
        if tail_bytes:
            rep["warnings"].append(
                "%d byte(s) of the unknown 112-byte tail differ; this tool "
                "does not interpret them" % tail_bytes)
    return rep


def list_file(path: str) -> dict:
    raw = read_bytes(path)
    key = parse_key(raw, path)
    name = require_canonical_name(key, path)
    table = load_symbols()
    rows = []
    for slot, (code, extra) in enumerate(key.actions):
        rows.append({
            "index": slot, "action": ACTION_NAMES[slot],
            "offset": slot_offset(slot), "code": code,
            "code_hex": "0x%04x" % code,
            "symbol": symbol_of(code, table),
            "extra": extra, "extra_hex": "0x%08x" % extra,
            "bytes_hex": raw[slot_offset(slot):slot_offset(slot) + PAIR_BYTES].hex(),
            "bound": code != 0,
        })
    return {"file": path, "bytes": len(raw), "name": name["text"],
            "name_hex": key.raw_name.hex(), "bound": key.bound,
            "slots": USED_SLOTS, "tail_bytes": TAIL_BYTES, "rows": rows,
            "problems": name["problems"], "warnings": name["warnings"],
            "symbols": len(table)}


def print_check(rep) -> None:
    print("check: %s (%d bytes)" % (rep["file"], rep["bytes"]))
    print("  name: %r  bound: %d/%d  tail: %d opaque bytes"
          % (rep["name"], rep["bound"], rep["slots"], rep["tail_bytes"]))
    print("  sha256: %s" % rep["sha256"][:16])
    if rep["against"]:
        a = rep["against"]
        print("  against: %s (%r)" % (a["file"], a["name"]))
        print("  changed: %d byte(s), first at offset %s; name %d, slots %s, "
              "tail %d"
              % (a["changed"],
                 a["first_diff"] if a["first_diff"] is not None else "n/a",
                 a["name_bytes"],
                 a["slots"] if a["slots"] else "none", a["tail_bytes"]))
    for problem in rep["problems"]:
        print("  problem: %s" % problem)
    for warning in rep["warnings"]:
        print("  warning: %s" % warning)
    print("  result: %s" % ("OK" if rep["ok"] else "FAILED"))


def print_list(rep) -> None:
    print("list: %s (%r, %d bytes, %d/%d bound, symbols: %s)"
          % (rep["file"], rep["name"], rep["bytes"], rep["bound"],
             rep["slots"],
             "%d" % rep["symbols"] if rep["symbols"] else "unavailable"))
    print("  %-3s %-18s %-8s %-8s %-10s %s"
          % ("idx", "action", "code", "symbol", "extra", "bytes"))
    for row in rep["rows"]:
        print("  %-3d %-18s %-8s %-8s %-10s %s"
              % (row["index"], row["action"], row["code_hex"], row["symbol"],
                 row["extra_hex"], " ".join(
                     row["bytes_hex"][i:i + 2]
                     for i in range(0, len(row["bytes_hex"]), 2))))
    for warning in rep["warnings"]:
        print("  warning: %s" % warning)


# ---------------------------------------------------------------------------
# --audit


def audit_corpus(directory: str = "") -> dict:
    directory = directory or os.path.join(GAME, "KEYCFG")
    if not os.path.isdir(directory):
        raise WriteError("not a directory: %s" % directory)
    files = key_files(directory)
    if not files:
        raise WriteError("no .key files under %s" % directory)
    names = []
    tails = []
    bytes_total = 0
    bound_per_file = []
    codes = []
    extras = Counter()
    name_lengths = Counter()
    errors = []
    for path in files:
        raw = read_bytes(path)
        bytes_total += len(raw)
        try:
            key = parse_key(raw, path)
        except WriteError as exc:
            errors.append(str(exc))
            continue
        info = inspect_name(key.raw_name)
        if info["problems"]:
            errors.append("%s: %s" % (path, "; ".join(info["problems"])))
        names.append(info["text"])
        name_lengths[len(info["text"])] += 1
        tails.append(key.tail)
        bound_per_file.append(key.bound)
        for code, extra in key.actions:
            if code:
                codes.append(code)
            extras[extra] += 1
    tail = tails[0] if tails else b""
    hist = Counter(tail)
    tail_identical = len(set(tails)) == 1
    return {
        "directory": directory, "files": len(files), "bytes": bytes_total,
        "names": names, "names_escaped": [esc(n.encode("latin1")) for n in names],
        "name_lengths": dict(sorted(name_lengths.items())),
        "unique_names": len({n.encode("latin1") for n in names}),
        "bound_per_file": bound_per_file,
        "used_slots": USED_SLOTS, "pairs_in_file": SLOT_COUNT,
        "bound": len(codes), "bound_total": len(files) * USED_SLOTS,
        "distinct_codes": len(set(codes)),
        "code_min": min(codes) if codes else 0,
        "code_max": max(codes) if codes else 0,
        "codes_outside_table": sum(1 for c in codes if c >= SYMBOL_COUNT),
        "extra_nonzero": sum(n for v, n in extras.items() if v),
        "extra_total": sum(extras.values()),
        "tail_bytes": TAIL_BYTES,
        "tail_identical": tail_identical,
        "tail_patterns": len(set(tails)),
        "tail_zero": tail.count(0),
        "tail_nonzero": len(tail) - tail.count(0),
        "tail_distinct_values": len(hist),
        "tail_top": hist.most_common(6),
        "tail_hex": tail.hex(),
        "errors": errors,
    }


def print_audit(rep) -> None:
    print("audit: %s" % rep["directory"])
    print("  files: %d, %d bytes; names: %s (lengths %s, unique %d)"
          % (rep["files"], rep["bytes"],
             ", ".join("%s(%d)" % (n, len(n))
                       for n in rep["names_escaped"]),
             rep["name_lengths"], rep["unique_names"]))
    print("  actions: %d used slots (pairs 0..%d of %d in the file); bound "
          "%d/%d (%.0f%%), per file %s"
          % (rep["used_slots"], rep["used_slots"] - 1, rep["pairs_in_file"],
             rep["bound"], rep["bound_total"],
             100.0 * rep["bound"] / max(1, rep["bound_total"]),
             "/".join(str(n) for n in rep["bound_per_file"])))
    print("  codes: distinct %d, range 0x%02x..0x%02x, outside the 123-entry "
          "name table: %d"
          % (rep["distinct_codes"], rep["code_min"], rep["code_max"],
             rep["codes_outside_table"]))
    print("  second u32 (modifiers?): nonzero %d/%d -> 'code + modifiers' is "
          "NOT confirmed" % (rep["extra_nonzero"], rep["extra_total"]))
    print("  tail (pairs %d..%d, %d bytes): identical in %d/%d pattern(s); "
          "zero %d, nonzero %d, distinct byte values %d"
          % (rep["used_slots"], rep["pairs_in_file"] - 1, rep["tail_bytes"],
             rep["tail_patterns"], rep["files"], rep["tail_zero"],
             rep["tail_nonzero"], rep["tail_distinct_values"]))
    print("  tail top bytes: %s"
          % ", ".join("%02x x%d" % (b, n) for b, n in rep["tail_top"]))
    print("  tail status: HYPOTHESIS - the writer's uninitialised stack "
          "(docs/CONTROLS_CPP.md §1.1); preserved verbatim, never edited")
    for error in rep["errors"]:
        print("  error: %s" % error)


# ---------------------------------------------------------------------------
# --roundtrip


def roundtrip_corpus(directory: str = "", verbose: bool = False) -> dict:
    if directory and os.path.isfile(directory):
        files = [directory]
        base = os.path.dirname(directory)
    else:
        base = directory or os.path.join(GAME, "KEYCFG")
        if not os.path.isdir(base):
            raise WriteError("not a directory: %s" % base)
        files = key_files(base)
    if not files:
        raise WriteError("no .key files under %s" % base)
    results = []
    total = 0
    for path in files:
        raw = read_bytes(path)
        total += len(raw)
        row = {"file": path, "bytes": len(raw)}
        try:
            key = parse_key(raw, path)
            again = key.to_bytes()
            oracle = smallmod.parse_key(raw)
            oracle_bytes = oracle.to_bytes()
            info = inspect_name(key.raw_name)
            row.update({
                "identical": again == raw,
                "oracle_identical": oracle_bytes == raw,
                "oracle_name": oracle.name,
                "own_name": key.name,
                "name_problems": info["problems"],
                "tail_same": len(again) == len(raw)
                             and again[TAIL_OFFSET:] == raw[TAIL_OFFSET:],
                "error": None,
            })
        except Exception as exc:                   # noqa: BLE001
            row.update({"identical": False, "oracle_identical": False,
                        "oracle_name": None, "own_name": None,
                        "name_problems": [], "tail_same": False,
                        "error": "%s: %s" % (type(exc).__name__, exc)})
        results.append(row)
        if verbose:
            print("  %-40s %s" % (os.path.basename(path),
                                  "identical" if row["identical"] else "DIFFERS"))
    identical = sum(1 for r in results if r["identical"])
    oracle_ok = sum(1 for r in results if r["oracle_identical"])
    errors = sum(1 for r in results if r["error"])
    name_problems = sum(1 for r in results if r["name_problems"])
    return {
        "directory": base, "files": len(files), "bytes": total,
        "identical": identical, "oracle_identical": oracle_ok,
        "errors": errors, "name_problems": name_problems,
        "tail_preserved": sum(1 for r in results if r["tail_same"]),
        "results": results,
    }


def print_roundtrip(rep) -> None:
    print("roundtrip: %s" % rep["directory"])
    print("  %d files, %d bytes" % (rep["files"], rep["bytes"]))
    print("  identical=%d/%d  differs=%d  errors=%d"
          % (rep["identical"], rep["files"], rep["files"] - rep["identical"],
             rep["errors"]))
    print("  tail preserved=%d/%d  non-canonical names=%d"
          % (rep["tail_preserved"], rep["files"], rep["name_problems"]))
    print("  oracle gm/small.py: to_bytes identical=%d/%d"
          % (rep["oracle_identical"], rep["files"]))
    print("result: %s"
          % ("OK" if rep["identical"] == rep["files"]
             and rep["oracle_identical"] == rep["files"]
             and not rep["errors"] else "FAILED"))


# ---------------------------------------------------------------------------
# --set-slot / --set-name


def parse_pair_hex(text: str) -> bytes:
    cleaned = text.strip()
    if cleaned.lower().startswith("0x"):
        cleaned = cleaned[2:]
    cleaned = cleaned.replace(" ", "").replace("_", "")
    if len(cleaned) != PAIR_BYTES * 2:
        raise WriteError("--bytes must be exactly %d hex digits (%d bytes, one "
                         "8-byte pair); got %r (%d digits)"
                         % (PAIR_BYTES * 2, PAIR_BYTES, text, len(cleaned)))
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        raise WriteError("--bytes %r is not hex" % text)


def apply_edit(path: str, out: str | None, slot: int | None = None,
               bytes_hex: str | None = None, name: str | None = None) -> dict:
    if not out:
        raise WriteError("--set-slot/--set-name need --out OUT.key")
    if slot is None and name is None:
        raise WriteError("nothing to do: give --set-slot N --bytes HEX and/or "
                         "--set-name TEXT")
    out = guard_out(out, path)
    raw = read_bytes(path)
    key = parse_key(raw, path)
    require_canonical_name(key, path)
    new_raw = bytearray(raw)
    edited = []

    if slot is not None:
        if bytes_hex is None:
            raise WriteError("--set-slot %d needs --bytes HEX (exactly 8 "
                             "bytes)" % slot)
        if slot < 0 or slot >= SLOT_COUNT:
            raise WriteError("--set-slot %d is out of range: the file stores "
                             "%d pairs (0..%d)"
                             % (slot, SLOT_COUNT, SLOT_COUNT - 1))
        if slot >= USED_SLOTS:
            raise WriteError(
                "refusing --set-slot %d: pairs %d..%d are the %d-byte tail "
                "whose meaning is not understood (docs/FORMATS.md §8.2, "
                "docs/CONTROLS_CPP.md §1.1); only slots 0..%d are actions"
                % (slot, USED_SLOTS, SLOT_COUNT - 1, TAIL_BYTES,
                   USED_SLOTS - 1))
        data = parse_pair_hex(bytes_hex)
        offset = slot_offset(slot)
        new_raw[offset:offset + PAIR_BYTES] = data
        edited.append({"kind": "slot", "slot": slot, "offset": offset,
                       "bytes": PAIR_BYTES, "hex": data.hex()})

    if name is not None:
        data = encode_name(name)
        new_raw[:NAME_SIZE] = data
        edited.append({"kind": "name", "offset": 0, "bytes": NAME_SIZE,
                       "hex": data.hex()})

    new_bytes = bytes(new_raw)
    if new_bytes[TAIL_OFFSET:] != raw[TAIL_OFFSET:]:
        raise WriteError("internal: the edit touched the opaque tail")

    redo = parse_key(new_bytes, out)
    if slot is not None:
        want = struct.unpack("<II", parse_pair_hex(bytes_hex))
        if redo.pairs[slot] != want:
            raise WriteError("internal: the slot does not read back")
    if name is not None and redo.name != name:
        raise WriteError("internal: the name does not read back")

    oracle = smallmod.parse_key(new_bytes)
    if oracle.to_bytes() != new_bytes:
        raise WriteError("internal: gm/small.py does not round-trip the output")
    if name is not None and oracle.name != name:
        raise WriteError("internal: gm/small.py does not see the new name")

    verdict = check_data(new_bytes, out, load_symbols())
    problems = verdict["problems"]
    if problems:
        raise WriteError("internal: the edited file fails --check: %s"
                         % "; ".join(problems))

    stat = diff_stats(raw, new_bytes)
    expected = 0
    if slot is not None:
        # bytes of the pair that actually change (0..8)
        expected += sum(1 for i in range(PAIR_BYTES)
                        if raw[slot_offset(slot) + i] !=
                        new_bytes[slot_offset(slot) + i])
    if name is not None:
        expected += sum(1 for i in range(NAME_SIZE)
                        if raw[i] != new_bytes[i])
    if stat["changed"] != expected:
        raise WriteError("internal: %d byte(s) changed, expected %d"
                         % (stat["changed"], expected))

    write_bytes(out, new_bytes)
    if read_bytes(out) != new_bytes:
        raise WriteError("internal: %s on disk does not match the report" % out)
    return {
        "file": path, "out": out, "edited": edited,
        "bytes_in": len(raw), "bytes_out": len(new_bytes),
        "changed_bytes": stat["changed"], "first_diff": stat["first_diff"],
        "delta": stat["delta"], "slot_bytes": PAIR_BYTES,
        "tail_untouched": True,
        "old_name": key.name, "new_name": redo.name,
        "sha256_in": sha256(raw), "sha256_out": sha256(new_bytes),
        "check_ok": verdict["ok"], "check_warnings": verdict["warnings"],
        "oracle_bytes": oracle.to_bytes() == new_bytes,
        "oracle_name": oracle.name,
    }


def print_edit(rep) -> None:
    print("edit: %s -> %s" % (rep["file"], rep["out"]))
    for item in rep["edited"]:
        if item["kind"] == "slot":
            print("  slot %d (%s): bytes %d..%d -> %s"
                  % (item["slot"], ACTION_NAMES[item["slot"]], item["offset"],
                     item["offset"] + item["bytes"] - 1, item["hex"]))
        else:
            print("  name: %r -> %r (name[16], %s)"
                  % (rep["old_name"], rep["new_name"], item["hex"]))
    print("  bytes: %d -> %d (%+d); changed %d byte(s), first at offset %s"
          % (rep["bytes_in"], rep["bytes_out"], rep["delta"],
             rep["changed_bytes"],
             rep["first_diff"] if rep["first_diff"] is not None else "n/a"))
    print("  tail (112 opaque bytes): untouched")
    print("  sha256: %s -> %s"
          % (rep["sha256_in"][:16], rep["sha256_out"][:16]))
    print("  gm/small.py sees %r; --check: %s"
          % (rep["oracle_name"], "ok" if rep["check_ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# --self-test


def _selftest_workdir() -> str:
    base = "/var/tmp"
    if not os.path.isdir(base) or not os.access(base, os.W_OK):
        base = "/tmp/opencode"
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = None
    return tempfile.mkdtemp(prefix="keywrite-selftest-", dir=base)


def synth_key(name: bytes = b"TEST", codes=(), second: int = 0,
              tail: bytes | None = None) -> bytes:
    """A synthetic 376-byte profile (no game files involved)."""
    if tail is None:
        tail = bytes(range(112))
    assert len(tail) == TAIL_BYTES
    out = bytearray(name[:NAME_SIZE].ljust(NAME_SIZE, b"\0"))
    for i in range(USED_SLOTS):
        out += struct.pack("<II", codes[i] if i < len(codes) else 0, second)
    return bytes(out) + tail


def _selftest(as_json: bool = False):
    results = []
    work = _selftest_workdir()

    def case(name, fn):
        try:
            detail = fn()
            results.append((name, True, detail or ""))
        except Exception as exc:                    # noqa: BLE001
            results.append((name, False, "%s: %s"
                            % (type(exc).__name__, exc)))

    def expect_error(fn, needle):
        try:
            fn()
        except WriteError as exc:
            if needle not in str(exc):
                raise AssertionError("message %r lacks %r" % (str(exc), needle))
            return "refused: %s" % str(exc)[:56]
        raise AssertionError("expected WriteError containing %r" % needle)

    def path(name, data=None):
        p = os.path.join(work, name)
        if data is not None:
            os.makedirs(os.path.dirname(p) or work, exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        return p

    good = synth_key(b"TEST", (0x11, 0x1F, 0x6F))
    table = load_symbols()

    def t_roundtrip():
        key = parse_key(good)
        assert key.to_bytes() == good
        assert len(good) == KEY_SIZE and len(key.pairs) == SLOT_COUNT
        assert key.name == "TEST" and key.bound == 3
        return "%d bytes, %d pairs, %d used" % (len(good), len(key.pairs),
                                                USED_SLOTS)

    def t_tail_preserved():
        key = parse_key(good)
        assert key.tail == good[TAIL_OFFSET:]
        assert len(key.tail) == TAIL_BYTES
        return "%d opaque bytes kept verbatim" % len(key.tail)

    def t_oracle_parse():
        oracle = smallmod.parse_key(good)
        assert len(oracle.entries) == SLOT_COUNT
        assert oracle.entries[0] == (0x11, 0)
        assert oracle.name == "TEST"
        return "gm/small.py reads the same pairs"

    def t_oracle_bytes():
        assert smallmod.parse_key(good).to_bytes() == good
        return "gm/small.py to_bytes is identical"

    def t_size_short():
        return expect_error(lambda: parse_key(good[:-1], "x"), "expected 376")

    def t_size_long():
        return expect_error(lambda: parse_key(good + b"\0", "x"), "expected 376")

    def t_name_no_nul():
        info = inspect_name(b"A" * 16)
        assert info["problems"], "16 letters must be a problem"
        assert "no NUL" in info["problems"][0]

        def strict():
            key = KeyFile(raw_name=b"A" * 16, pairs=[(0, 0)] * SLOT_COUNT)
            require_canonical_name(key, "noNUL.key")

        return expect_error(strict, "no NUL")

    def t_name_nul_inside():
        raw = b"AB\0CD" + b"\0" * 11
        info = inspect_name(raw)
        assert info["problems"] and "non-NUL" in info["problems"][0]
        return "NUL inside the name rejected"

    def t_name_control_warn():
        info = inspect_name(b"A\x01B" + b"\0" * 13)
        assert not info["problems"] and info["warnings"]
        return "control characters are a warning, not a refusal"

    def t_name_empty_warn():
        info = inspect_name(b"\0" * 16)
        assert not info["problems"] and "empty" in info["warnings"][0]
        return "empty name is legal + warned"

    def t_encode_name_ok():
        assert encode_name("PERFECT") == b"PERFECT" + b"\0" * 9
        assert len(encode_name("A" * 15)) == NAME_SIZE
        return "<=15 bytes + NUL"

    def t_encode_name_long():
        return expect_error(lambda: encode_name("A" * 16), "at most 15")

    def t_encode_name_nul():
        return expect_error(lambda: encode_name("A\0B"), "NUL")

    def t_encode_name_unicode():
        return expect_error(lambda: encode_name("\u0416"), "latin1")

    def t_pair_hex():
        assert parse_pair_hex("1d00000000000000") == \
            struct.pack("<II", 0x1D, 0)
        assert parse_pair_hex("0x11 00 00 00 00 00 00 00") == \
            bytes([0x11] + [0] * 7)
        return "8-byte pair parsing"

    def t_pair_hex_short():
        return expect_error(lambda: parse_pair_hex("1d000000"), "exactly 16")

    def t_pair_hex_bad():
        return expect_error(lambda: parse_pair_hex("zz" * 8), "not hex")

    def t_set_slot_eight():
        src = path("a.key", good)
        out = path("out/a.key")
        rep = apply_edit(src, out, slot=5, bytes_hex="1d00000000000000")
        assert rep["changed_bytes"] == 1, rep["changed_bytes"]
        assert rep["tail_untouched"] and rep["oracle_bytes"]
        edited = read_bytes(out)
        assert edited[slot_offset(5):slot_offset(5) + 8] == \
            struct.pack("<II", 0x1D, 0)
        assert parse_key(edited).pairs[5] == (0x1D, 0)
        return "slot 5, 1 byte changed (0x00 -> 0x1d)"

    def t_set_slot_full_eight():
        src = path("b.key", good)
        out = path("out/b.key")
        rep = apply_edit(src, out, slot=0, bytes_hex="ffffff7f01000000")
        assert rep["changed_bytes"] == 5, rep["changed_bytes"]
        assert parse_key(read_bytes(out)).pairs[0] == (0x7FFFFFFF, 1)
        return "code+second written, 5 bytes differ"

    def t_set_slot_corrupt_refused():
        src = path("b2.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(work, "out/b2.key"), slot=0,
                               bytes_hex="0000008000000000"), "top bit set")

    def t_set_slot_offset():
        assert slot_offset(0) == 16 and slot_offset(30) == 256
        assert slot_offset(31) == TAIL_OFFSET
        return "slot n at 16 + 8n"

    def t_set_slot_tail_refused():
        src = path("c.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(work, "out/c.key"),
                               slot=31, bytes_hex="00" * 8), "tail")

    def t_set_slot_range():
        src = path("d.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(work, "out/d.key"),
                               slot=45, bytes_hex="00" * 8), "out of range")

    def t_set_slot_negative():
        src = path("e.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(work, "out/e.key"),
                               slot=-1, bytes_hex="00" * 8), "out of range")

    def t_set_slot_needs_bytes():
        src = path("f.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(work, "out/f.key"), slot=3),
            "needs --bytes")

    def t_set_name():
        src = path("g.key", good)
        out = path("out/g.key")
        rep = apply_edit(src, out, name="NEWSET")
        edited = read_bytes(out)
        assert edited[:NAME_SIZE] == b"NEWSET" + b"\0" * 10
        assert edited[NAME_SIZE:] == good[NAME_SIZE:]
        assert rep["changed_bytes"] == 5, rep["changed_bytes"]
        assert parse_key(edited).name == "NEWSET"
        return "name only, 3 bytes changed, slots untouched"

    def t_set_both():
        src = path("h.key", good)
        out = path("out/h.key")
        rep = apply_edit(src, out, slot=2, bytes_hex="3900000000000000",
                         name="BOTH")
        assert rep["changed_bytes"] >= 2 and rep["tail_untouched"]
        key = parse_key(read_bytes(out))
        assert key.pairs[2] == (0x39, 0) and key.name == "BOTH"
        return "slot + name in one pass"

    def t_set_noop_refused():
        src = path("i.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(work, "out/i.key")),
            "nothing to do")

    def t_set_needs_out():
        src = path("j.key", good)
        return expect_error(lambda: apply_edit(src, None, slot=0,
                                               bytes_hex="00" * 8), "--out")

    def t_guard_game():
        src = path("k.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(DEFAULT_GAME, "KEYCFG",
                                                 "X.key"), slot=0,
                               bytes_hex="00" * 8), "read-only")

    def t_guard_mirror():
        src = path("l.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(MIRROR_GAME, "KEYCFG",
                                                 "X.key"), slot=0,
                               bytes_hex="00" * 8), "read-only")

    def t_guard_repo():
        src = path("m.key", good)
        return expect_error(
            lambda: apply_edit(src, os.path.join(ROOT, "tmp", "x.key"), slot=0,
                               bytes_hex="00" * 8), "read-only")

    def t_guard_same():
        src = path("n.key", good)
        return expect_error(
            lambda: apply_edit(src, src, slot=0, bytes_hex="00" * 8),
            "overwrite the input")

    def t_check_ok():
        p = path("ok.key", good)
        rep = check_file(p)
        assert rep["ok"] and not rep["problems"], rep["problems"]
        assert rep["bytes"] == KEY_SIZE and rep["bound"] == 3
        return "structure ok"

    def t_check_diff():
        a = path("base.key", good)
        b = path("out/diff.key")
        apply_edit(a, b, slot=4, bytes_hex="2500000000000000")
        rep = check_file(b, against=a)
        assert rep["ok"] and rep["against"]["changed"] == 1
        assert rep["against"]["slots"] == [4]
        assert rep["against"]["name_bytes"] == 0
        assert rep["against"]["tail_bytes"] == 0
        return "1 changed byte, slot 4, tail 0"

    def t_check_diff_name():
        a = path("base2.key", good)
        b = path("out/name.key")
        apply_edit(a, b, name="ABC")
        rep = check_file(b, against=a)
        assert rep["against"]["name_bytes"] == 4, rep["against"]["name_bytes"]
        assert rep["against"]["slots"] == []
        return "name diff counted separately"

    def t_check_corrupt():
        codes = [0x80000001]
        p = path("bad.key", synth_key(b"BAD", codes))
        rep = check_file(p)
        assert not rep["ok"] and rep["corrupt_slots"] == [0]
        return "top-bit code is a problem"

    def t_check_unknown():
        p = path("unk.key", synth_key(b"UNK", (0x200,)))
        rep = check_file(p)
        assert rep["ok"] and rep["unknown_slots"] == [0]
        assert any("outside the 123" in w for w in rep["warnings"])
        return "unknown code preserved + warning"

    def t_check_extra_warn():
        p = path("extra.key", synth_key(b"EXT", (0x11,), second=7))
        rep = check_file(p)
        assert rep["ok"] and any("second u32" in w for w in rep["warnings"])
        return "nonzero second u32 warned, not interpreted"

    def t_check_size():
        p = path("short.key", good[:-1])
        return expect_error(lambda: check_file(p), "expected 376")

    def t_check_bad_name():
        p = path("noname.key", synth_key(b"A" * 16))
        return expect_error(lambda: check_file(p), "no NUL")

    def t_list_rows():
        p = path("list.key", synth_key(b"LIST", (0x11, 0, 0x6D)))
        rep = list_file(p)
        assert len(rep["rows"]) == USED_SLOTS
        assert rep["rows"][0]["action"] == "forward"
        assert rep["rows"][0]["bytes_hex"] == "1100000000000000"
        assert rep["rows"][30]["action"] == "linear_right"
        assert rep["rows"][30]["offset"] == 256
        return "31 rows, offsets 16..256"

    def t_list_symbols():
        if len(table) != SYMBOL_COUNT:
            return "controls.cpp table unavailable (display fallback only)"
        assert symbol_of(0, table) == "NONE"
        assert table[0x11] == "W" and table[0x39] == "SPACE"
        assert table[0x6D] == "UP"
        assert symbol_of(999, table) == "KEY_999"
        return "0x11=W, 0x39=SPACE, 0x6d=UP"

    def t_esc():
        assert esc(b"A\x00\xff") == "A\\x00\\xff"
        assert esc(b"AB C") == "AB C"
        return "byte-faithful display"

    def t_diff_helper():
        assert diff_stats(b"abc", b"axc") == {"changed": 1, "first_diff": 1,
                                              "delta": 0}
        assert diff_stats(b"abc", b"ab") == {"changed": 1, "first_diff": 2,
                                             "delta": -1}
        assert sha256(b"") == ("e3b0c44298fc1c149afbf4c8996fb924"
                               "27ae41e4649b934ca495991b7852b855")
        return "diff/sha helpers"

    def t_audit_synth():
        d = os.path.join(work, "corpus")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "ONE.key"), "wb") as f:
            f.write(synth_key(b"ONE", (0x11, 0x1F, 0x6F)))
        with open(os.path.join(d, "TWO.KEY"), "wb") as f:
            f.write(synth_key(b"TWO", (0x11,)))
        rep = audit_corpus(d)
        assert rep["files"] == 2 and rep["bytes"] == 2 * KEY_SIZE
        assert rep["bound"] == 4 and rep["bound_per_file"] == [3, 1]
        assert rep["distinct_codes"] == 3
        assert rep["tail_identical"] and rep["tail_patterns"] == 1
        assert rep["extra_nonzero"] == 0
        return "2 files, 4/62 bound, tail identical 2/2"

    def t_roundtrip_synth():
        d = os.path.join(work, "rt")
        os.makedirs(d, exist_ok=True)
        for name, codes in (("A.key", (0x11,)), ("B.KEY", (0x6D, 0x72))):
            with open(os.path.join(d, name), "wb") as f:
                f.write(synth_key(name[:2].encode(), codes))
        rep = roundtrip_corpus(d)
        assert rep["identical"] == 2 and rep["oracle_identical"] == 2
        assert rep["tail_preserved"] == 2 and not rep["errors"]
        return "identical=2/2, oracle 2/2"

    def t_roundtrip_file():
        p = path("single.key", good)
        rep = roundtrip_corpus(p)
        assert rep["files"] == 1 and rep["identical"] == 1
        return "single-file mode"

    def t_roundtrip_no_files():
        return expect_error(
            lambda: roundtrip_corpus(os.path.join(work, "empty")),
            "not a directory")

    for name, fn in [
            ("roundtrip: parse/to_bytes", t_roundtrip),
            ("roundtrip: tail verbatim", t_tail_preserved),
            ("oracle: same pairs", t_oracle_parse),
            ("oracle: to_bytes identical", t_oracle_bytes),
            ("size: 375 refused", t_size_short),
            ("size: 377 refused", t_size_long),
            ("name: 16 bytes no NUL", t_name_no_nul),
            ("name: NUL inside", t_name_nul_inside),
            ("name: control chars warn", t_name_control_warn),
            ("name: empty warns", t_name_empty_warn),
            ("name: <=15 + NUL", t_encode_name_ok),
            ("name: 16 bytes refused", t_encode_name_long),
            ("name: NUL refused", t_encode_name_nul),
            ("name: non-latin1 refused", t_encode_name_unicode),
            ("pair: hex parsing", t_pair_hex),
            ("pair: short hex refused", t_pair_hex_short),
            ("pair: bad hex refused", t_pair_hex_bad),
            ("slot: one byte changed", t_set_slot_eight),
            ("slot: code+second written", t_set_slot_full_eight),
            ("slot: corrupt code refused", t_set_slot_corrupt_refused),
            ("slot: offset 16 + 8n", t_set_slot_offset),
            ("slot: tail 31 refused", t_set_slot_tail_refused),
            ("slot: 45 out of range", t_set_slot_range),
            ("slot: -1 out of range", t_set_slot_negative),
            ("slot: needs --bytes", t_set_slot_needs_bytes),
            ("name: --set-name only", t_set_name),
            ("edit: slot + name", t_set_both),
            ("edit: no operation", t_set_noop_refused),
            ("edit: needs --out", t_set_needs_out),
            ("guard: game", t_guard_game),
            ("guard: mirror", t_guard_mirror),
            ("guard: repository", t_guard_repo),
            ("guard: same file", t_guard_same),
            ("check: ok", t_check_ok),
            ("check: slot diff", t_check_diff),
            ("check: name diff", t_check_diff_name),
            ("check: corrupt code", t_check_corrupt),
            ("check: unknown code warns", t_check_unknown),
            ("check: second u32 warns", t_check_extra_warn),
            ("check: bad size refused", t_check_size),
            ("check: bad name refused", t_check_bad_name),
            ("list: 31 rows", t_list_rows),
            ("list: symbols", t_list_symbols),
            ("helpers: escaping", t_esc),
            ("helpers: sha/diff", t_diff_helper),
            ("audit: synthetic corpus", t_audit_synth),
            ("roundtrip: synthetic corpus", t_roundtrip_synth),
            ("roundtrip: single file", t_roundtrip_file),
            ("roundtrip: empty dir", t_roundtrip_no_files),
    ]:
        case(name, fn)

    shutil.rmtree(work, ignore_errors=True)
    passed = sum(1 for _n, ok, _d in results if ok)
    if as_json:
        print(json.dumps({
            "mode": "self-test", "passed": passed, "total": len(results),
            "ok": passed == len(results),
            "checks": [{"name": n, "ok": ok, "detail": d}
                       for n, ok, d in results],
        }, indent=1))
    else:
        for name, ok, detail in results:
            print("  %-32s %s  %s" % (name, "ok  " if ok else "FAIL", detail))
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
    ok = (rep["identical"] == rep["files"]
          and rep["oracle_identical"] == rep["files"] and not rep["errors"])
    return 0 if ok else 1


def _cmd_audit(args) -> int:
    rep = audit_corpus(args.audit or "")
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_audit(rep)
    return 1 if rep["errors"] else 0


def _cmd_list(args) -> int:
    rep = list_file(args.list)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_list(rep)
    return 1 if rep["problems"] else 0


def _cmd_check(args) -> int:
    rep = check_file(args.check, args.against)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_check(rep)
    return 0 if rep["ok"] else 1


def _cmd_set(args) -> int:
    if not args.file:
        raise WriteError("--set-slot/--set-name need --file IN.key")
    if not args.out:
        raise WriteError("--set-slot/--set-name need --out OUT.key")
    rep = apply_edit(args.file, args.out, args.set_slot, args.bytes,
                     args.set_name)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_edit(rep)
    return 0 if rep["check_ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(
        description="Writer/editor for the Gunmetal .key key profiles (M11.5).")
    ap.add_argument("--roundtrip", nargs="?", const="", metavar="DIR|FILE",
                    help="read and rebuild every .key, compare byte for byte "
                         "(default game/KEYCFG, recursive)")
    ap.add_argument("--audit", nargs="?", const="", metavar="DIR",
                    help="corpus numbers: names, 31 used slots, 112-byte tail "
                         "(default game/KEYCFG)")
    ap.add_argument("--list", metavar="FILE",
                    help="the 31 actions with code, symbol and bytes")
    ap.add_argument("--check", metavar="FILE", help="structural check")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: also diff against FILE (changed bytes)")
    ap.add_argument("--file", metavar="FILE",
                    help="input profile for --set-slot/--set-name")
    ap.add_argument("--set-slot", type=int, metavar="N",
                    help="replace one 8-byte action pair (0..30); 31..44 is "
                         "the unknown tail and is refused")
    ap.add_argument("--bytes", metavar="HEX",
                    help="the new 8 bytes for --set-slot (16 hex digits)")
    ap.add_argument("--set-name", metavar="TEXT",
                    help="replace name[16] (latin1, <=15 bytes + NUL)")
    ap.add_argument("--out", metavar="PATH", help="output file (never the game)")
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
    if len([x for x in (args.roundtrip, args.audit, args.list, args.check,
                        args.set_slot, args.set_name) if x is not None]) > 1:
        ap.error("choose one of --roundtrip/--audit/--list/--check/"
                 "--set-slot/--set-name")
    if args.roundtrip is not None:
        return _cmd_roundtrip(args)
    if args.audit is not None:
        return _cmd_audit(args)
    if args.list:
        return _cmd_list(args)
    if args.check:
        return _cmd_check(args)
    if args.set_slot is not None or args.set_name is not None:
        return _cmd_set(args)
    if args.file:
        raise WriteError("--file needs --set-slot N --bytes HEX and/or "
                         "--set-name TEXT")
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except WriteError as exc:
        print("keywrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, OSError) as exc:
        print("keywrite: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                        # noqa: BLE001
        print("keywrite: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
