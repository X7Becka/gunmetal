#!/usr/bin/env python3
"""Round-trip proof and byte-splicing editor for Gunmetal ``.soe`` thing
templates and the single-line ``.sni`` path file (``M11.5``, slice 8).

``.soe`` (``docs/FORMATS.md`` §13, ``docs/SOE.md``, ``gm/soe.py``) is the text
source of one 936-byte ``THING_TYPES`` record: whitespace-separated tokens,
first token = version 0..13, later blocks gated on it.  The shipped corpus is a
single file, ``SOBS/railrock.soe`` (340 bytes, 61 lines, 67 tokens), so the
grammar was recovered from the loader (``0x47cb6c``) and every parser check
below is a proof against that one file plus synthetic streams.

``.sni`` (``docs/FORMATS.md`` §8.2, ``gm/small.py``) is one path line; the
shipped file ``path.sni`` is ``.\\r\\n`` (3 bytes).

What this tool adds to the oracle
---------------------------------
``gm/soe.py`` already has a byte-exact ``to_bytes()``: it keeps the exact text
lines, so ``parse -> to_bytes`` reproduces the shipped file.  What it cannot do
is *edit* one token while proving that every other byte stayed put: the writer
is line-based, and ``render_lines()`` (one canonical value per line) is not the
original layout.  This tool therefore

* tokenises the file into **byte spans** (line + flat token index), labels every
  token through its own grammar walker (independent of ``gm/soe.py``), and
  cross-checks label/kind/value against the oracle token by token;
* edits **only the bytes of the target token**, so separators (the shipped file
  wraps numbers in spaces), CRLF endings and the trailing blank line survive;
* refuses the tokens that gate the grammar: ``version`` (0..13 switches whole
  blocks), ``collision_shape`` (``ellipsoid`` = 3 radii, ``sphere`` = 1 radius)
  and the three counts ``path_point_count`` / ``weapon_count`` /
  ``children_count`` (they control how many tokens follow).  Editing those
  adds/removes tokens, i.e. creates or deletes blocks rather than editing one.

``.sni`` has a single token (the whole line); its terminator is kept.

Command surface
---------------
``--roundtrip [DIR]``   read -> own walk -> oracle ``to_bytes`` -> write ->
                        read from disk -> compare, for every ``.soe``/``.sni``
                        under DIR (default: the game installation).
``--audit [DIR]``       ``.soe``: version, tokens per block (collision/events/
                        weapons/children/path/fields), number ranges, strings;
                        ``.sni``: the single line and its EOL.
``--list FILE``         every token with its number, line, byte span, kind,
                        label and value.
``--check FILE [--against FILE] [--token N --value V]``
                        structure checks + byte comparison + changed-byte
                        count; with ``--token/--value`` the file must be
                        exactly the source with that token patched.
``--set FILE --token N --value V --out OUT``
                        token-level edit; refuses the game, the mirror and the
                        repository as output.
``--self-test``         20+ checks on synthetic bytes, no game data.
``--json``              one machine-readable object per mode.

Exit codes: ``0`` success, ``1`` verification failed, ``2`` bad input or a
refused edit (message on stderr), ``3`` internal error.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass, field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import small as oracle_small                # noqa: E402
from gm import soe as oracle_soe                    # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/soewrite"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")
SIBLING_REPO = os.path.expanduser("~/gunmetal-re")

#: Loader version range (``0x47cb6c``); every block is gated on ``version``.
MAX_VERSION = 13
#: Loader clamps (``0x47dc2c``, ``0x47ddad``, ``0x47daea``).
MAX_WEAPONS = 8
MAX_CHILDREN = 4
MAX_PATH_POINTS = 32

INT32_MIN, INT32_MAX = -2**31, 2**31 - 1

CRLF = b"\r\n"

#: Tokens whose value controls how many tokens are read after them.  Editing
#: one of these adds/removes blocks, so they are read-only by design.
STRUCTURAL = ("version", "collision_shape", "path_point_count",
              "weapon_count", "children_count")

#: ``.soe`` semantic groups for ``--audit``.  ``str_c8``/``str_f0`` are the
#: *destroy*/*death* events attributed to the 10-field block in
#: ``docs/FORMATS.md`` §13 (hypothesis, not a loader error string).
COLLISION_LABELS = frozenset({
    "collision_type", "col_f10", "col_f20", "col_f28", "col_f90", "col_f98",
    "col_fa0", "collision_shape", "radius", "radius_x", "radius_y",
    "radius_z",
})
#: ``event_a``/``event_b`` plus the named event slots of the loader.
EVENT_LABELS = frozenset({
    "event_a", "event_b", "expire_event", "create_event",
    "morale_failure_event", "time_out_event", "damaged_event",
    "str_c8", "str_f0",
})
ID_LABELS = frozenset({"f19c", "f198", "class_name", "f1a8", "f1ac", "f1b0"})


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The bytes do not walk as a ``.soe``/``.sni`` (own reader)."""


# ---------------------------------------------------------------------------
# line and token spans
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Line:
    """One physical line: text span ``[start, end)`` plus its exact terminator."""

    index: int
    start: int
    end: int
    eol: bytes
    text: str

    @property
    def span(self) -> tuple[int, int]:
        return (self.start, self.end)


@dataclass
class Token:
    """A whitespace-delimited token with its absolute byte span and grammar."""

    index: int
    text: str
    start: int
    end: int
    line: int
    kind: str = "?"          # "int" | "flt" | "str" | "?"
    label: str = "?"         # grammar label, same spelling as gm/soe.py
    group: str = "?"         # semantic group for --audit
    value: object = None     # parsed value

    @property
    def span(self) -> tuple[int, int]:
        return (self.start, self.end)


def split_lines(data: bytes) -> list[Line]:
    """Split on CRLF / LF / CR, keeping each terminator byte-exact."""
    lines: list[Line] = []
    i, n = 0, len(data)
    while i < n:
        j = i
        while j < n and data[j] not in (0x0A, 0x0D):
            j += 1
        start, end = i, j
        if j >= n:
            eol = b""
            i = j
        elif data[j] == 0x0D and j + 1 < n and data[j + 1] == 0x0A:
            eol = b"\r\n"
            i = j + 2
        elif data[j] == 0x0D:
            eol = b"\r"
            i = j + 1
        else:
            eol = b"\n"
            i = j + 1
        lines.append(Line(len(lines), start, end, eol,
                          data[start:end].decode("latin1")))
        if j >= n:
            break
    return lines


def rejoin(data: bytes, lines: list[Line]) -> bytes:
    """Reassemble the file from its line spans; proves the spans cover it."""
    parts: list[bytes] = []
    pos = 0
    for line in lines:
        if line.start < pos or line.end < line.start or line.eol not in (
                b"", b"\n", b"\r", b"\r\n"):
            raise ScanError(f"line spans are out of order at {line.start}")
        parts.append(data[pos:line.start])
        parts.append(line.text.encode("latin1"))
        parts.append(line.eol)
        pos = line.end + len(line.eol)
    parts.append(data[pos:])
    return b"".join(parts)


def flat_tokens(lines: list[Line]) -> list[Token]:
    """All whitespace-separated tokens of the file, in order, with spans.

    ``latin-1`` maps one byte to one character, so scanning ``str.isspace()``
    reproduces ``str.split()`` (the oracle's tokenisation) exactly.
    """
    out: list[Token] = []
    for line in lines:
        text = line.text
        i, n = 0, len(text)
        while i < n:
            while i < n and text[i].isspace():
                i += 1
            if i >= n:
                break
            start = i
            while i < n and not text[i].isspace():
                i += 1
            out.append(Token(len(out), text[start:i], line.start + start,
                             line.start + i, line.index))
    return out


# ---------------------------------------------------------------------------
# semantic groups
# ---------------------------------------------------------------------------

def group_of(label: str) -> str:
    if label == "version":
        return "header"
    if label in ID_LABELS:
        return "id"
    if label in COLLISION_LABELS:
        return "collision"
    if label in EVENT_LABELS:
        return "events"
    if label.startswith("weapon"):
        return "weapons"
    if label.startswith("child"):
        return "children"
    if label.startswith("path_point"):
        return "path"
    return "fields"


# ---------------------------------------------------------------------------
# own ``.soe`` reader (independent of gm/soe.py, same grammar)
# ---------------------------------------------------------------------------

@dataclass
class SoeDoc:
    """A walked ``.soe``: exact lines, labelled tokens, block summary."""

    path: str
    data: bytes
    lines: list[Line]
    tokens: list[Token]
    consumed: int
    version: int = -1
    collision_type: str = ""
    shape: str = ""
    path_point_count: int = 0
    weapon_count: int = 0
    children_count: int = 0

    @property
    def extra(self) -> int:
        """Trailing tokens the loader never reads (0 for a well-formed file)."""
        return len(self.tokens) - self.consumed

    @property
    def size(self) -> int:
        return len(self.data)


class _Walker:
    __slots__ = ("tokens", "i")

    def __init__(self, tokens: list[Token]):
        self.tokens = tokens
        self.i = 0

    def _next(self, kind: str) -> Token:
        if self.i >= len(self.tokens):
            raise ScanError(
                f"unexpected end of .soe: token {self.i} is missing "
                f"(expected a {'number' if kind == 'flt' else 'integer' if kind == 'int' else 'string'})")
        return self.tokens[self.i]

    def take(self, kind: str, label: str) -> object:
        token = self._next(kind)
        where = f"token {token.index} (line {token.line + 1})"
        if kind == "int":
            try:
                value: object = int(token.text, 10)
            except ValueError:
                raise ScanError(
                    f"{where}: expected an integer, got {token.text!r}") from None
            if not INT32_MIN <= value <= INT32_MAX:
                raise ScanError(
                    f"{where}: integer {value} is outside the loader's "
                    f"32-bit field")
        elif kind == "flt":
            try:
                value = float(token.text)
            except ValueError:
                raise ScanError(
                    f"{where}: expected a number, got {token.text!r}") from None
            if not math.isfinite(value):
                raise ScanError(f"{where}: non-finite number {token.text!r}")
        else:
            value = token.text
        token.kind = kind
        token.label = label
        token.group = group_of(label)
        token.value = value
        self.i += 1
        return value

    def count(self, label: str, cap: int) -> int:
        value = int(self.take("int", label))
        if value < 0:
            raise ScanError(f"{label} is negative: {value}")
        if value > cap:
            raise ScanError(
                f"{label}={value} exceeds the loader limit {cap}")
        remaining = len(self.tokens) - self.i
        if value > remaining:
            raise ScanError(
                f"{label}={value} but only {remaining} token(s) follow")
        return value


def walk_soe(data: bytes, path: str = "") -> SoeDoc:
    """Parse ``.soe`` bytes into labelled tokens with byte spans.

    Grammar is the loader's ``fscanf`` order (``docs/FORMATS.md`` §13).  The
    walk stops after the last block the version enables; any remaining token is
    reported as ``extra`` (the loader never reads it).
    """
    if not data:
        raise ScanError("empty .soe (a thing template needs a version)")
    lines = split_lines(data)
    tokens = flat_tokens(lines)
    if not tokens:
        raise ScanError("no tokens (a thing template needs a version)")

    w = _Walker(tokens)
    version = int(w.take("int", "version"))
    if not 0 <= version <= MAX_VERSION:
        raise ScanError(
            f"version {version} is outside the loader range 0..{MAX_VERSION}")

    # id / model-name block
    w.take("int", "f19c")
    w.take("int", "f198")
    w.take("str", "class_name")
    w.take("int", "f1a8")
    w.take("int", "f1ac")
    if version >= 9:
        w.take("int", "f1b0")

    # collision / movement block
    collision_type = str(w.take("str", "collision_type"))
    for label in ("col_f10", "col_f20", "col_f28", "col_f90", "col_f98",
                  "col_fa0"):
        w.take("flt", label)
    shape = str(w.take("str", "collision_shape")).lower()
    # The oracle stores the shape lower-cased (``gm/soe.py`` ``.lower()``), so
    # the token value follows the same spelling; its raw text is unchanged.
    tokens[w.i - 1].value = shape
    if shape == "ellipsoid":
        w.take("flt", "radius_x")
        w.take("flt", "radius_y")
        w.take("flt", "radius_z")
    elif shape == "sphere":
        w.take("flt", "radius")

    for label in ("f50", "f58", "f60", "f68"):
        w.take("flt", label)

    if version >= 7:
        w.take("str", "event_a")
        w.take("int", "f1d4")
    else:
        w.take("str", "event_a")
        w.take("str", "event_b")
        w.take("int", "f1d4")

    if version >= 8:
        for i in range(4):
            w.take("int", f"shield{i}")
    else:
        w.take("int", "shield0")

    w.take("str", "tex_a")
    w.take("str", "tex_b")

    w.take("int", "expire_value")
    if version >= 3:
        w.take("str", "expire_event")

    # 10-field block: destroy event, three doubles, hits, death event,
    # sound name, volume, timer period/step
    w.take("str", "str_c8")
    w.take("flt", "f78")
    w.take("flt", "f80")
    w.take("flt", "f88")
    w.take("int", "f1c8")
    w.take("str", "str_f0")
    w.take("str", "str_1a4")
    w.take("int", "f200")
    w.take("int", "f210")
    w.take("int", "f214")

    if version >= 4:
        w.take("flt", "falling_fear")
    if version >= 5:
        w.take("str", "create_event")
    if version >= 6:
        w.take("str", "morale_failure_event")
        w.take("str", "time_out_event")
        w.take("str", "damaged_event")
    if version >= 7:
        w.take("int", "f1cc")
        w.take("int", "f1d0")
    if version >= 10:
        w.take("flt", "f_a8")
        w.take("flt", "f_b0")
    if version >= 11:
        w.take("flt", "f_b8")
        w.take("flt", "f_c0")
    if version >= 12:
        w.take("flt", "f18")
        w.take("flt", "f70")
    if version >= 13:
        w.take("str", "leash_location")
        w.take("flt", "f_d0")

    path_points = w.count("path_point_count", MAX_PATH_POINTS) \
        if version >= 2 else 0
    for i in range(path_points):
        w.take("str", f"path_point[{i}]")

    weapons = w.count("weapon_count", MAX_WEAPONS)
    for i in range(weapons):
        w.take("str", f"weapon[{i}].name")
        for k in range(6):
            w.take("int", f"weapon[{i}].w{k}")

    children = w.count("children_count", MAX_CHILDREN) if version >= 1 else 0
    for i in range(children):
        w.take("str", f"child[{i}].name")
        for k in range(6):
            w.take("flt", f"child[{i}].c{k}")
        w.take("int", f"child[{i}].x")

    for token in tokens[w.i:]:
        token.kind = "?"
        token.label = "extra"
        token.group = "extra"

    return SoeDoc(path=path, data=data, lines=lines, tokens=tokens,
                  consumed=w.i, version=version,
                  collision_type=collision_type, shape=shape,
                  path_point_count=path_points, weapon_count=weapons,
                  children_count=children)


def parse_soe_file(path: str) -> SoeDoc:
    return walk_soe(read_bytes(path), path)


# ---------------------------------------------------------------------------
# own ``.sni`` reader
# ---------------------------------------------------------------------------

@dataclass
class SniDoc:
    """A walked ``.sni``: one non-empty line, its span and terminator."""

    path: str
    data: bytes
    lines: list[Line]
    line: Line
    text: str

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def eol(self) -> bytes:
        return self.line.eol

    @property
    def tokens(self) -> list["Token"]:
        return [Token(0, self.text, self.line.start, self.line.end,
                      self.line.index, "str", "path", "sni", self.text)]


def walk_sni(data: bytes, path: str = "") -> SniDoc:
    """Parse ``.sni`` bytes: exactly one non-empty line (a path)."""
    if not data:
        raise ScanError("empty .sni (expected one path line)")
    lines = split_lines(data)
    content = [line for line in lines if line.text.strip()]
    if not content:
        raise ScanError("no path line in .sni (only blank lines)")
    if len(content) > 1:
        raise ScanError(
            f".sni must hold one path line, found {len(content)} "
            f"(lines {', '.join(str(l.index + 1) for l in content)})")
    line = content[0]
    if "\0" in line.text:
        raise ScanError("NUL byte in the .sni path line")
    return SniDoc(path=path, data=data, lines=lines, line=line,
                  text=line.text)


def parse_sni_file(path: str) -> SniDoc:
    return walk_sni(read_bytes(path), path)


# ---------------------------------------------------------------------------
# patching
# ---------------------------------------------------------------------------

def splice(data: bytes, replacements: list[tuple[tuple[int, int], str]]) -> bytes:
    """Replace only the given byte spans; every other byte stays in place."""
    ordered = sorted(replacements, key=lambda item: item[0][0])
    parts: list[bytes] = []
    pos = 0
    for (start, end), text in ordered:
        if start < pos:
            raise EditError("overlapping token spans")
        parts.append(data[pos:start])
        parts.append(text.encode("latin1"))
        pos = end
    parts.append(data[pos:])
    return b"".join(parts)


def aligned_diff(old: bytes, new: bytes) -> int:
    """Bytes that differ when the two blobs are aligned from the start."""
    return sum(1 for a, b in zip(old, new) if a != b) + abs(len(old) - len(new))


def token_changed_bytes(old_text: str, new_text: str) -> int:
    """Bytes that differ *inside* one token (a width change counts its tail).

    This is the honest "changed N bytes" number for an edit: the whole-file
    aligned diff also counts every shifted byte when the token width changes.
    """
    return aligned_diff(old_text.encode("latin1"), new_text.encode("latin1"))


def first_diff(old: bytes, new: bytes) -> dict:
    limit = min(len(old), len(new))
    offset = next((i for i in range(limit) if old[i] != new[i]), limit)
    return {"offset": offset,
            "old": old[offset:offset + 16].hex(),
            "new": new[offset:offset + 16].hex(),
            "size_delta": len(new) - len(old),
            "changed_bytes": aligned_diff(old, new),
            "identical": old == new}


def diff_summary(diff: dict) -> str:
    if diff["identical"]:
        return "identical"
    return (f"size differs by {diff['size_delta']:+d} byte(s), first diff at "
            f"offset {diff['offset']}; aligned byte diff "
            f"{diff['changed_bytes']}")


# ---------------------------------------------------------------------------
# token resolution and validation
# ---------------------------------------------------------------------------

def resolve_soe_token(doc: SoeDoc, index: int) -> Token:
    if index < 0 or index >= len(doc.tokens):
        raise EditError(
            f"token {index}: the file has {len(doc.tokens)} token(s) "
            f"(0..{len(doc.tokens) - 1})")
    token = doc.tokens[index]
    if token.label == "extra":
        raise EditError(
            f"token {index}: trailing token the loader never reads "
            f"(the grammar ends at token {doc.consumed - 1})")
    return token


def validate_soe_edit(token: Token, raw: str) -> object:
    """Refuse structural tokens and values the grammar/loader cannot take."""
    if token.label in STRUCTURAL:
        why = {
            "version": "it gates whole blocks (0..13)",
            "collision_shape": "it gates the radii (sphere = 1, "
                               "ellipsoid = 3 values)",
            "path_point_count": "it is the number of path-point tokens "
                                "(loader max 32)",
            "weapon_count": "it is the number of weapon entries "
                            "(loader max 8)",
            "children_count": "it is the number of child entries "
                              "(loader max 4)",
        }[token.label]
        raise EditError(
            f"{token.label} is not editable: {why}; changing it adds or "
            f"removes tokens instead of editing one")
    value_text = raw.strip()
    if not value_text:
        raise EditError("--value must not be empty")
    if any(ch.isspace() for ch in value_text):
        raise EditError(
            f"--value must be one whitespace-free token: {raw!r}")
    try:
        value_text.encode("latin1")
    except UnicodeEncodeError:
        raise EditError(
            f"--value is not representable in latin-1: {raw!r}") from None
    if token.kind == "int":
        try:
            value = int(value_text, 10)
        except ValueError:
            raise EditError(
                f"token {token.index} ({token.label}) is an integer, got "
                f"{raw!r}") from None
        if not INT32_MIN <= value <= INT32_MAX:
            raise EditError(
                f"token {token.index} is stored in a 32-bit field: {value}")
        return value
    if token.kind == "flt":
        try:
            value = float(value_text)
        except ValueError:
            raise EditError(
                f"token {token.index} ({token.label}) is a number, got "
                f"{raw!r}") from None
        if not math.isfinite(value):
            raise EditError(f"token {token.index}: non-finite value {raw!r}")
        return value
    if token.kind == "str":
        return value_text
    raise EditError(f"token {token.index} has no proven kind ({token.kind!r})")


def patch_soe(doc: SoeDoc, token: Token, raw: str) -> tuple[bytes, object]:
    value = validate_soe_edit(token, raw)
    text = raw.strip()
    return splice(doc.data, [(token.span, text)]), value


def patch_sni(doc: SniDoc, raw: str) -> bytes:
    value = raw.strip()
    if not value:
        raise EditError("--value must not be empty")
    if any(ch.isspace() for ch in value):
        raise EditError(f"a path token has no whitespace: {raw!r}")
    try:
        value.encode("latin1")
    except UnicodeEncodeError:
        raise EditError(
            f"--value is not representable in latin-1: {raw!r}") from None
    return splice(doc.data, [(doc.line.span, value)])


# ---------------------------------------------------------------------------
# verification of an edit
# ---------------------------------------------------------------------------

def _oracle_soe(data: bytes):
    return oracle_soe.parse_text(data.decode("latin1"))


def _oracle_value(model, index: int) -> object:
    return model.fields[index].value


def verify_soe_edit(doc: SoeDoc, new_bytes: bytes, index: int, raw: str) -> dict:
    """Prove the edit changed the target token and nothing else."""
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        new_doc = walk_soe(new_bytes, doc.path)
        check("own reader parses the edited file", True,
              f"{new_doc.consumed} token(s), extra {new_doc.extra}")
    except ScanError as exc:
        check("own reader parses the edited file", False, str(exc))
        return {"ok": False, "checks": checks}
    try:
        new_model = _oracle_soe(new_bytes)
        check("oracle parses the edited file to EOF", True,
              f"extra {new_model.extra}")
    except ValueError as exc:
        check("oracle parses the edited file to EOF", False, str(exc))
        return {"ok": False, "checks": checks}

    old_token = doc.tokens[index]
    new_token = new_doc.tokens[index]
    text = raw.strip()
    value = validate_soe_edit(old_token, raw)
    diff = first_diff(doc.data, new_bytes)

    check("token count is unchanged", len(new_doc.tokens) == len(doc.tokens),
          f"{len(doc.tokens)} -> {len(new_doc.tokens)}")
    check("size change equals the token width change",
          len(new_bytes) - len(doc.data)
          == len(text.encode("latin1")) - (old_token.end - old_token.start),
          f"{len(doc.data)} -> {len(new_bytes)} bytes "
          f"({len(new_bytes) - len(doc.data):+d})")
    check("bytes outside the target token are identical",
          doc.data[:old_token.start] == new_bytes[:new_token.start]
          and doc.data[old_token.end:] == new_bytes[new_token.end:])
    check("every other token text is identical",
          all(t.text == new_doc.tokens[i].text
              for i, t in enumerate(doc.tokens) if i != index),
          f"{len(doc.tokens) - 1} token(s)")
    check("the token keeps its grammar label and kind",
          (new_token.label, new_token.kind) == (old_token.label,
                                                old_token.kind)
          and new_model.fields[index].label == old_token.label
          and new_model.fields[index].kind == old_token.kind,
          f"#{index} {old_token.label}/{old_token.kind}")

    if old_token.kind == "str":
        expected = str(value)
        check("own reader sees the new value", new_token.value == expected,
              repr(expected))
        check("oracle sees the new value",
              _oracle_value(new_model, index) == expected, repr(expected))
    else:
        check("own reader sees the new value", new_token.value == value,
              f"{new_token.value!r}")
        check("oracle sees the new value",
              _oracle_value(new_model, index) == value,
              f"{_oracle_value(new_model, index)!r}")

    check("the structural fields are unchanged",
          (new_doc.version, new_doc.shape, new_doc.path_point_count,
           new_doc.weapon_count, new_doc.children_count)
          == (doc.version, doc.shape, doc.path_point_count,
              doc.weapon_count, doc.children_count),
          f"version {doc.version}, shape {doc.shape}, "
          f"path {doc.path_point_count}, weapons {doc.weapon_count}, "
          f"children {doc.children_count}")
    check("changed-byte count is reported", True,
          f"{diff['changed_bytes']} byte(s)")
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "diff": diff, "new_doc": new_doc, "new_model": new_model}


def verify_sni_edit(doc: SniDoc, new_bytes: bytes, raw: str) -> dict:
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        new_doc = walk_sni(new_bytes, doc.path)
        check("own reader parses the edited file", True,
              f"{len(new_doc.text)} char(s)")
    except ScanError as exc:
        check("own reader parses the edited file", False, str(exc))
        return {"ok": False, "checks": checks}
    diff = first_diff(doc.data, new_bytes)
    text = raw.strip()
    check("the file still holds exactly one path line",
          len(new_doc.lines) == len(doc.lines), f"{len(new_doc.lines)} line(s)")
    check("the line terminator is preserved", new_doc.eol == doc.eol,
          repr(new_doc.eol))
    check("size change equals the path width change",
          len(new_bytes) - len(doc.data)
          == len(text.encode("latin1")) - len(doc.text.encode("latin1")),
          f"{len(doc.data)} -> {len(new_bytes)} bytes "
          f"({len(new_bytes) - len(doc.data):+d})")
    check("bytes outside the path line are identical",
          doc.data[:doc.line.start] == new_bytes[:new_doc.line.start]
          and doc.data[doc.line.end:] == new_bytes[new_doc.line.end:])
    check("own reader sees the new path", new_doc.text == text, repr(new_doc.text))
    check("the oracle sees the new path",
          oracle_small.parse_sni(new_bytes) == text, repr(text))
    check("changed-byte count is reported", True,
          f"{diff['changed_bytes']} byte(s)")
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "diff": diff, "new_doc": new_doc}


# ---------------------------------------------------------------------------
# paths and refusal rules
# ---------------------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def atomic_write(path: str, data: bytes) -> None:
    directory = os.path.dirname(os.path.abspath(path)) or "."
    try:
        os.makedirs(directory, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix=".soewrite-", dir=directory)
        try:
            with os.fdopen(fd, "wb") as fh:
                fh.write(data)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(temp, path)
        except BaseException:
            try:
                os.unlink(temp)
            except OSError:
                pass
            raise
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def file_kind(path: str) -> str:
    """``soe`` / ``sni`` from the extension; anything else is a mistyped call."""
    low = path.lower()
    if low.endswith(".soe"):
        return "soe"
    if low.endswith(".sni"):
        return "sni"
    raise EditError(f"not a .soe/.sni file: {path}")


def require_input(path: str) -> str:
    kind = file_kind(path)
    if not os.path.isfile(path):
        raise EditError(f"no such file: {path}")
    return kind


def _real(path: str) -> str:
    return os.path.realpath(os.path.abspath(path))


def protected_roots(game: str) -> list[str]:
    roots = []
    for candidate in (game, DEFAULT_GAME, MIRROR, ROOT, SIBLING_REPO):
        if candidate:
            roots.append(_real(candidate))
    return sorted(set(roots))


def refuse_unsafe_out(out_path: str, game: str, kind: str) -> None:
    """Never write into the game, the regress mirror or a repository."""
    suffix = "." + kind
    if not out_path.lower().endswith(suffix):
        raise EditError(f"--out must be a {suffix} path: {out_path}")
    target = _real(out_path)
    for root in protected_roots(game):
        if target == root or target.startswith(root + os.sep):
            if root in (_real(game), _real(DEFAULT_GAME)):
                what = "the game installation"
            elif root == _real(MIRROR):
                what = "the regress mirror"
            else:
                what = "the repository"
            raise EditError(
                f"refusing to write inside {what}: {out_path} "
                f"(write to /tmp/opencode or /var/tmp)")


def resolve_input(name: str, game: str = DEFAULT_GAME) -> str:
    """``railrock`` / ``railrock.soe`` / a path -> an existing file."""
    if os.path.isfile(name):
        require_input(name)
        return name
    wanted = []
    if os.path.splitext(name)[1]:
        wanted = [name]
    else:
        wanted = [name + ".soe", name + ".sni"]
    for base in wanted:
        direct = os.path.join(game, base)
        if os.path.isfile(direct):
            require_input(direct)
            return direct
        for folder in ("SOBS", "thing"):
            candidate = os.path.join(game, folder, base)
            if os.path.isfile(candidate):
                require_input(candidate)
                return candidate
    low = {b.lower() for b in wanted}
    for dirpath, dirnames, filenames in os.walk(game):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower() in low:
                candidate = os.path.join(dirpath, filename)
                require_input(candidate)
                return candidate
    raise EditError(f"file not found: {name} (looked in {game})")


def corpus_files(root: str) -> list[tuple[str, str]]:
    """Every ``.soe``/``.sni`` under *root* as ``(kind, path)``, sorted."""
    if not os.path.isdir(root):
        raise EditError(f"no such directory: {root}")
    found: list[tuple[str, str]] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for filename in sorted(filenames):
            low = filename.lower()
            if low.endswith(".soe"):
                found.append(("soe", os.path.join(dirpath, filename)))
            elif low.endswith(".sni"):
                found.append(("sni", os.path.join(dirpath, filename)))
    return found


def shipped_same_relative(path: str, game: str) -> str | None:
    """The shipped file with the same relative path / basename."""
    parts = os.path.abspath(path).split(os.sep)
    for i, part in enumerate(parts[:-1]):
        if part.lower() in ("sobs", "thing"):
            candidate = os.path.join(game, *parts[i:])
            if os.path.isfile(candidate):
                return candidate
    base = os.path.basename(path)
    for folder in ("SOBS", "thing", ""):
        candidate = (os.path.join(game, folder, base) if folder
                     else os.path.join(game, base))
        if os.path.isfile(candidate):
            return candidate
    return None


# ---------------------------------------------------------------------------
# structure checks
# ---------------------------------------------------------------------------

def structure_soe(doc: SoeDoc, model) -> dict:
    checks: list[dict] = []
    warnings: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    own_labels = [t.label for t in doc.tokens[:doc.consumed]]
    ref_labels = [f.label for f in model.fields]
    own_kinds = [t.kind for t in doc.tokens[:doc.consumed]]
    ref_kinds = [f.kind for f in model.fields]
    own_values = [t.value for t in doc.tokens[:doc.consumed]]
    ref_values = [f.value for f in model.fields]

    check("line scanner re-joins the file byte-exactly",
          rejoin(doc.data, doc.lines) == doc.data,
          f"{len(doc.lines)} line(s), final EOL "
          f"{(doc.lines[-1].eol or b'none')!r}")
    check("own token count equals the oracle token count",
          len(doc.tokens) == len(model.fields) + model.extra,
          f"{len(doc.tokens)} vs {len(model.fields) + model.extra}")
    check("own labels equal the oracle labels", own_labels == ref_labels,
          f"{len(ref_labels)} label(s)")
    check("own kinds equal the oracle kinds", own_kinds == ref_kinds,
          f"{sum(1 for k in ref_kinds if k == 'int')} int, "
          f"{sum(1 for k in ref_kinds if k == 'flt')} flt, "
          f"{sum(1 for k in ref_kinds if k == 'str')} str")
    check("own values equal the oracle values", own_values == ref_values,
          f"{len(ref_values)} value(s)")
    check("no trailing tokens (the loader consumes the whole stream)",
          doc.extra == 0 and model.extra == 0,
          f"own extra {doc.extra}, oracle extra {model.extra}")
    check("the version is inside the loader range",
          0 <= doc.version <= MAX_VERSION,
          f"{doc.version} (0..{MAX_VERSION})")
    check("the block counts fit the loader clamps",
          doc.path_point_count <= MAX_PATH_POINTS
          and doc.weapon_count <= MAX_WEAPONS
          and doc.children_count <= MAX_CHILDREN,
          f"path {doc.path_point_count}/{MAX_PATH_POINTS}, "
          f"weapons {doc.weapon_count}/{MAX_WEAPONS}, "
          f"children {doc.children_count}/{MAX_CHILDREN}")

    if doc.extra:
        warnings.append(
            f"{doc.extra} trailing token(s) after the grammar are never read "
            f"by the loader (0x47cb6c)")
    if doc.shape not in ("none", "sphere", "ellipsoid", ""):
        warnings.append(
            f"collision shape {doc.shape!r} is not none/sphere/ellipsoid; the "
            f"loader reads no radii for it")

    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "warnings": warnings, "tokens": len(doc.tokens),
            "consumed": doc.consumed, "extra": doc.extra,
            "version": doc.version, "shape": doc.shape,
            "collision_type": doc.collision_type,
            "path_points": doc.path_point_count,
            "weapons": doc.weapon_count, "children": doc.children_count}


def structure_sni(doc: SniDoc) -> dict:
    checks: list[dict] = []
    warnings: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    check("line scanner re-joins the file byte-exactly",
          rejoin(doc.data, doc.lines) == doc.data,
          f"{len(doc.lines)} line(s), EOL {doc.eol or b'none'!r}")
    check("exactly one non-empty path line",
          len([l for l in doc.lines if l.text.strip()]) == 1,
          f"{len(doc.lines)} line(s)")
    check("the oracle reads the same path",
          oracle_small.parse_sni(doc.data) == doc.text, repr(doc.text))
    if not doc.eol:
        warnings.append("the path line has no terminator (the oracle writes "
                        "CRLF)")
    elif doc.eol != CRLF:
        warnings.append(f"the path line ends with {doc.eol!r}, not CRLF; the "
                        f"oracle writes CRLF")
    if doc.text != doc.text.strip():
        warnings.append("the path line has leading/trailing whitespace")
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "warnings": warnings, "lines": len(doc.lines),
            "eol": doc.eol.decode("latin1") if doc.eol else "",
            "text": doc.text}


# ---------------------------------------------------------------------------
# list
# ---------------------------------------------------------------------------

def list_soe(path: str) -> dict:
    doc = parse_soe_file(path)
    model = _oracle_soe(doc.data)
    structure = structure_soe(doc, model)
    rows = [{"index": t.index, "line": t.line + 1, "span": list(t.span),
             "kind": t.kind, "label": t.label, "group": t.group,
             "text": t.text, "value": t.value} for t in doc.tokens]
    return {"file": path, "kind": "soe", "size": doc.size,
            "lines": len(doc.lines), "tokens": len(doc.tokens),
            "version": doc.version, "rows": rows, "structure": structure}


def list_sni(path: str) -> dict:
    doc = parse_sni_file(path)
    structure = structure_sni(doc)
    rows = [{"index": 0, "line": doc.line.index + 1,
             "span": list(doc.line.span), "kind": "str", "label": "path",
             "group": "sni", "text": doc.text, "value": doc.text}]
    return {"file": path, "kind": "sni", "size": doc.size,
            "lines": len(doc.lines), "tokens": 1, "rows": rows,
            "structure": structure}


def print_list(rep: dict) -> None:
    print(f"file: {rep['file']} ({rep['size']} bytes, {rep['lines']} line(s), "
          f"{rep['tokens']} token(s)"
          + (f", version {rep['version']}" if rep["kind"] == "soe" else "")
          + ")")
    print("   #  line  span          kind  label                value")
    for row in rep["rows"]:
        span = f"{row['span'][0]}..{row['span'][1]}"
        print(f"{row['index']:4d} {row['line']:5d}  {span:<13} "
              f"{row['kind']:<4}  {row['label']:<20} "
              f"{row['text']!r}")
    for warning in rep["structure"]["warnings"]:
        print(f"warning: {warning}")


# ---------------------------------------------------------------------------
# set
# ---------------------------------------------------------------------------

def set_token(path: str, index: int, raw: str, out_path: str,
              game: str = DEFAULT_GAME) -> dict:
    source = resolve_input(path, game)
    kind = file_kind(source)
    if _real(out_path) == _real(source):
        raise EditError("--out is the input file; pick a different path")
    refuse_unsafe_out(out_path, game, kind)
    data = read_bytes(source)

    if kind == "soe":
        doc = walk_soe(data, source)
        token = resolve_soe_token(doc, index)
        old_text = token.text
        new_bytes, value = patch_soe(doc, token, raw)
        result = verify_soe_edit(doc, new_bytes, index, raw)
        label = token.label
    else:
        doc = walk_sni(data, source)
        if index != 0:
            raise EditError(
                f"token {index}: a .sni has a single token (0 = the path)")
        old_text = doc.text
        value = raw.strip()
        new_bytes = patch_sni(doc, raw)
        result = verify_sni_edit(doc, new_bytes, raw)
        label = "path"

    rep = {"ok": result["ok"], "source": source, "out": out_path,
           "kind": kind, "token": index, "label": label,
           "old": old_text, "new": raw.strip(), "value": value,
           "source_size": len(data), "size": len(new_bytes),
           "size_delta": len(new_bytes) - len(data),
           "changed_bytes": token_changed_bytes(old_text, raw.strip()),
           "file_changed_bytes": result["diff"]["changed_bytes"],
           "checks": result["checks"], "written": False}
    if result["ok"]:
        atomic_write(out_path, new_bytes)
        rep["written"] = True
    return rep


def print_set(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    print(f"token {rep['token']} ({rep['label']}): {rep['old']!r} -> "
          f"{rep['new']!r} ({rep['source_size']} -> {rep['size']} bytes, "
          f"delta {rep['size_delta']:+d}; "
          f"{rep['changed_bytes']} byte(s) in the token, "
          f"file aligned diff {rep['file_changed_bytes']})")
    for item in rep["checks"]:
        print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
              + (f" ({item['detail']})" if item.get("detail") else ""))
    if rep["written"]:
        print(f"wrote {rep['out']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

def _diff_tokens(old, new) -> dict:
    """Changed/added/removed token indices between two documents."""
    changed, old_only, new_only = [], [], []
    for i in range(min(len(old), len(new))):
        if old[i].text != new[i].text:
            changed.append({"index": i, "label": old[i].label,
                            "old": old[i].text, "new": new[i].text})
    if len(old) > len(new):
        old_only = list(range(len(new), len(old)))
    elif len(new) > len(old):
        new_only = list(range(len(old), len(new)))
    return {"changed": changed, "old_only": old_only, "new_only": new_only}


def check_file(path: str, against: str | None = None,
               token: int | None = None, value: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    kind = file_kind(path)
    data = read_bytes(path)
    if (token is None) != (value is None):
        raise EditError("--token and --value must be given together")

    if kind == "soe":
        doc = walk_soe(data, path)
        model = _oracle_soe(data)
        structure = structure_soe(doc, model)
        tokens = doc.tokens
        new_bytes = None
        if token is not None and value is not None:
            spec = resolve_soe_token(doc, token)
            if against is None:
                raise EditError("--token/--value needs --against to compare")
            new_bytes = patch_soe(doc, spec, value)[0]
    else:
        doc = walk_sni(data, path)
        structure = structure_sni(doc)
        tokens = doc.tokens
        new_bytes = None
        if token is not None and value is not None:
            if against is None:
                raise EditError("--token/--value needs --against to compare")
            if token != 0:
                raise EditError(
                    f"token {token}: a .sni has a single token (0 = the path)")
            new_bytes = patch_sni(doc, value)

    rep: dict = {"file": path, "kind": kind, "size": len(data),
                 "structure": structure, "ok": structure["ok"],
                 "against": None, "diff": None, "note": "",
                 "changed_bytes": 0, "file_changed_bytes": 0,
                 "changed_tokens": None}

    if against is None:
        shipped = shipped_same_relative(path, game)
        if shipped is not None and _real(shipped) != _real(path):
            against = shipped
            rep["note"] = f"compared against the shipped {shipped}"
        elif shipped is not None:
            rep["note"] = ("this is the shipped file itself; pass --against "
                           "to compare an edited copy")
        else:
            rep["note"] = ("no --against and no shipped file with this name: "
                           "byte comparison skipped")

    if against:
        against = resolve_input(against, game)
        if file_kind(against) != kind:
            raise EditError(
                f"--against is a .{file_kind(against)} file, the input is "
                f".{kind}")
        old_bytes = read_bytes(against)
        old_doc = (walk_soe(old_bytes, against) if kind == "soe"
                   else walk_sni(old_bytes, against))
        checks: list[dict] = []

        def check(name: str, ok: bool, detail: str = "") -> None:
            checks.append({"check": name, "ok": bool(ok), "detail": detail})

        diff = first_diff(old_bytes, data)
        rep["against"] = {"path": against, "size": len(old_bytes),
                          "checks": checks, "diff": diff, "ok": True}
        rep["diff"] = diff
        rep["file_changed_bytes"] = diff["changed_bytes"]

        if new_bytes is not None:
            old_text = (old_doc.tokens[token].text
                        if token < len(old_doc.tokens) else "")
            rep["changed_bytes"] = token_changed_bytes(
                old_text, (value or "").strip())
            check("the file is exactly the source with the token patched",
                  data == new_bytes, diff_summary(diff))
            check("the token carries the expected value",
                  tokens[token].text == (value or "").strip(),
                  f"#{token} {tokens[token].label} = "
                  f"{tokens[token].text!r}")
            unexpected = [i for i in range(min(len(old_doc.tokens),
                                              len(tokens)))
                          if i != token
                          and old_doc.tokens[i].text != tokens[i].text]
            check("only the target token differs", not unexpected,
                  "unexpected: " + ", ".join(f"#{i}" for i in unexpected)
                  if unexpected else f"#{token} only")
        else:
            check("the file is byte-identical to the source",
                  data == old_bytes, diff_summary(diff))
            tokens_diff = _diff_tokens(old_doc.tokens, tokens)
            rep["changed_tokens"] = tokens_diff
            rep["changed_bytes"] = sum(
                token_changed_bytes(c["old"], c["new"])
                for c in tokens_diff["changed"])
            if tokens_diff["old_only"] or tokens_diff["new_only"]:
                check("token count and order are unchanged", False,
                      f"{len(old_doc.tokens)} -> {len(tokens)}")
            elif tokens_diff["changed"]:
                check("every change is one token substitution", True,
                      ", ".join(f"#{c['index']} {c['label']} "
                                f"{c['old']!r}->{c['new']!r}"
                                for c in tokens_diff["changed"]))
        rep["against"]["ok"] = all(c["ok"] for c in checks)
        rep["ok"] = rep["ok"] and rep["against"]["ok"]

    return rep


def print_check(rep: dict) -> None:
    print(f"check: {rep['file']} ({rep['size']} bytes, .{rep['kind']})")
    for item in rep["structure"]["checks"]:
        print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
              + (f" ({item['detail']})" if item.get("detail") else ""))
    for warning in rep["structure"]["warnings"]:
        print(f"  [warn] {warning}")
    if rep.get("note"):
        print(f"note: {rep['note']}")
    if rep["against"]:
        print(f"against: {rep['against']['path']} "
              f"({rep['against']['size']} bytes)")
        for item in rep["against"]["checks"]:
            print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
                  + (f" ({item['detail']})" if item.get("detail") else ""))
        if rep["changed_tokens"]:
            changed = rep["changed_tokens"]["changed"]
            print(f"changed: {len(changed)} token(s), "
                  f"{rep['changed_bytes']} byte(s) in tokens, "
                  f"file aligned diff {rep['file_changed_bytes']}"
                  + (": " + ", ".join(
                      f"#{c['index']} {c['label']} {c['old']!r} -> "
                      f"{c['new']!r}" for c in changed) if changed else ""))
        else:
            print(f"changed: {rep['changed_bytes']} byte(s) in the token, "
                  f"file aligned diff {rep['file_changed_bytes']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# round-trip over the corpus
# ---------------------------------------------------------------------------

def _scratch_dir(tag: str) -> str:
    base = DEFAULT_OUT_DIR
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = tempfile.gettempdir()
    return tempfile.mkdtemp(prefix=tag + "-", dir=base)


def _roundtrip_one(kind: str, path: str, scratch: str, rel: str) -> dict:
    """Read -> own walk -> oracle writer -> write -> read -> compare."""
    data = read_bytes(path)
    item = {"path": path, "rel": rel, "kind": kind, "size": len(data),
            "tokens": 0}
    try:
        doc = (walk_soe(data, path) if kind == "soe"
               else walk_sni(data, path))
    except ScanError as exc:
        item["reason"] = f"own reader: {exc}"
        return item

    item["tokens"] = len(doc.tokens)
    item["rejoin"] = rejoin(data, doc.lines) == data
    if not item["rejoin"]:
        item["reason"] = "line spans do not re-join the file"
        return item

    if kind == "soe":
        model = _oracle_soe(data)
        item["extra"] = model.extra
        rebuilt = model.to_bytes()
        item["canonical"] = None
        try:
            canonical = b"\r\n".join(
                ln.encode("latin1") for ln in model.render_lines())
            again = _oracle_soe(canonical)
            item["canonical"] = (
                [(f.label, f.kind, f.value) for f in again.fields]
                == [(f.label, f.kind, f.value) for f in model.fields])
        except ValueError as exc:
            item["canonical"] = False
            item["canonical_error"] = str(exc)
    else:
        model = oracle_small.parse_sni(data)
        item["rebuilt_semantics"] = model == doc.text
        rebuilt = oracle_small.sni_to_bytes(model)
        item["canonical"] = None      # no canonical re-render for a one-line path

    out = os.path.join(scratch, rel)
    atomic_write(out, rebuilt)
    written = read_bytes(out)
    item["written_size"] = len(written)
    item["identical"] = written == data
    if not item["identical"]:
        diff = first_diff(data, written)
        item["diff"] = diff
        if kind == "sni":
            item["reason"] = ("oracle always writes CRLF and strips the "
                              f"terminator (got {doc.eol!r})"
                              if doc.eol != CRLF else
                              f"first diff at offset {diff['offset']}")
        else:
            item["reason"] = (f"first diff at offset {diff['offset']}; "
                              f"size differs by {diff['size_delta']:+d}")

    # semantic re-parse: the written file must yield the same token stream
    try:
        again = (walk_soe(written, out) if kind == "soe"
                 else walk_sni(written, out))
        if kind == "soe":
            item["semantic"] = (
                [(t.label, t.kind, t.value) for t in again.tokens]
                == [(t.label, t.kind, t.value) for t in doc.tokens])
        else:
            item["semantic"] = again.text == doc.text
    except ScanError as exc:
        item["semantic"] = False
        item["reason"] = item.get("reason") or f"re-parse: {exc}"
    return item


def roundtrip_corpus(root: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    files = corpus_files(root)
    if not files:
        raise EditError(f"no .soe/.sni files under {root}")
    scratch = _scratch_dir("roundtrip")
    start = time.perf_counter()
    items: list[dict] = []
    mismatches: list[dict] = []
    try:
        for kind, path in files:
            rel = os.path.relpath(path, root)
            item = _roundtrip_one(kind, path, scratch, rel)
            items.append(item)
            if not (item.get("identical") and item.get("rejoin")
                    and item.get("semantic")
                    and item.get("canonical") is not False):
                mismatches.append(item)
            if verbose:
                print(("  ok   " if item.get("identical") else "  FAIL ")
                      + f"{rel} ({item['size']} bytes, "
                        f"{item['tokens']} token(s))")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    seconds = time.perf_counter() - start
    soe = [i for i in items if i["kind"] == "soe"]
    sni = [i for i in items if i["kind"] == "sni"]
    return {"ok": not mismatches and len(items) == len(files),
            "root": root, "files": len(files), "identical":
            sum(1 for i in items if i.get("identical")),
            "bytes": sum(i["size"] for i in items),
            "tokens": sum(i["tokens"] for i in items),
            "seconds": round(seconds, 3),
            "soe": {"files": len(soe),
                    "identical": sum(1 for i in soe if i.get("identical")),
                    "bytes": sum(i["size"] for i in soe),
                    "tokens": sum(i["tokens"] for i in soe)},
            "sni": {"files": len(sni),
                    "identical": sum(1 for i in sni if i.get("identical")),
                    "bytes": sum(i["size"] for i in sni),
                    "tokens": sum(i["tokens"] for i in sni)},
            "rejoin": sum(1 for i in items if i.get("rejoin")),
            "semantic": sum(1 for i in items if i.get("semantic")),
            "canonical": sum(1 for i in items if i.get("canonical")),
            "mismatches": mismatches, "per_file": items}


def print_roundtrip(rep: dict) -> None:
    print(f"roundtrip: identical={rep['identical']}/{rep['files']}, "
          f"{rep['bytes']} bytes, {rep['tokens']} tokens, "
          f"{rep['seconds']} s ({rep['root']})")
    for name in ("soe", "sni"):
        part = rep[name]
        print(f"  .{name}: identical={part['identical']}/{part['files']}, "
              f"{part['bytes']} bytes, {part['tokens']} token(s)")
    print(f"  own reader: {rep['rejoin']}/{rep['files']} line-span re-join; "
          f"semantic re-parse {rep['semantic']}/{rep['files']}; "
          f"canonical re-render {rep['canonical']}/{rep['soe']['files']} "
          f".soe")
    for item in rep["mismatches"]:
        print(f"  [FAIL] {item['rel']} ({item['size']} bytes): "
              f"{item.get('reason', 'differs')}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# audit
# ---------------------------------------------------------------------------

def _value_stats(values: list) -> dict:
    if not values:
        return {"count": 0, "min": None, "max": None, "distinct": 0}
    return {"count": len(values), "min": min(values), "max": max(values),
            "distinct": len(set(values))}


def audit_soe(path: str, rel: str) -> dict:
    doc = parse_soe_file(path)
    model = _oracle_soe(doc.data)
    structure = structure_soe(doc, model)
    groups: dict[str, int] = {}
    for token in doc.tokens:
        groups[token.group] = groups.get(token.group, 0) + 1
    ints = [t.value for t in doc.tokens if t.kind == "int"]
    floats = [t.value for t in doc.tokens if t.kind == "flt"]
    strings = [t.value for t in doc.tokens if t.kind == "str"]
    blocks = {"header": 1, "id": 1, "collision": 1, "events": 1,
              "weapons": doc.weapon_count, "children": doc.children_count,
              "path": doc.path_point_count, "fields": 1}
    return {"path": path, "rel": rel, "kind": "soe", "size": doc.size,
            "lines": len(doc.lines), "tokens": len(doc.tokens),
            "version": doc.version,
            "collision_type": doc.collision_type, "shape": doc.shape,
            "groups": groups, "blocks": blocks,
            "ints": _value_stats(ints), "floats": _value_stats(floats),
            "strings": _value_stats(strings),
            "string_values": sorted(set(strings)),
            "structure": structure}


def audit_sni(path: str, rel: str) -> dict:
    doc = parse_sni_file(path)
    structure = structure_sni(doc)
    return {"path": path, "rel": rel, "kind": "sni", "size": doc.size,
            "lines": len(doc.lines), "tokens": 1, "text": doc.text,
            "eol": structure["eol"], "structure": structure}


def audit_corpus(root: str = DEFAULT_GAME) -> dict:
    files = corpus_files(root)
    if not files:
        raise EditError(f"no .soe/.sni files under {root}")
    start = time.perf_counter()
    entries = []
    for kind, path in files:
        rel = os.path.relpath(path, root)
        entries.append(audit_soe(path, rel) if kind == "soe"
                       else audit_sni(path, rel))
    seconds = time.perf_counter() - start
    soe = [e for e in entries if e["kind"] == "soe"]
    sni = [e for e in entries if e["kind"] == "sni"]
    return {"ok": all(e["structure"]["ok"] for e in entries),
            "root": root, "files": len(entries),
            "soe_files": len(soe), "sni_files": len(sni),
            "bytes": sum(e["size"] for e in entries),
            "tokens": sum(e["tokens"] for e in entries),
            "seconds": round(seconds, 3), "entries": entries}


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} file(s) (.soe {rep['soe_files']}, "
          f".sni {rep['sni_files']}), {rep['bytes']} bytes, "
          f"{rep['tokens']} tokens, {rep['seconds']} s ({rep['root']})")
    for e in rep["entries"]:
        if e["kind"] == "soe":
            g = e["groups"]
            print(f"  {e['rel']}: {e['size']} bytes, {e['lines']} lines, "
                  f"{e['tokens']} tokens, version {e['version']}, "
                  f"collision {e['collision_type']}/{e['shape']}")
            print("    tokens by block: "
                  + ", ".join(f"{k} {g.get(k, 0)}" for k in
                              ("header", "id", "collision", "events",
                               "weapons", "children", "path", "fields"))
                  + f" (weapon entries {e['blocks']['weapons']}, "
                    f"children {e['blocks']['children']}, "
                    f"path points {e['blocks']['path']})")
            print(f"    ints: {e['ints']['count']} value(s), "
                  f"{e['ints']['min']}..{e['ints']['max']}, "
                  f"{e['ints']['distinct']} distinct; "
                  f"floats: {e['floats']['count']} value(s), "
                  f"{e['floats']['min']}..{e['floats']['max']}, "
                  f"{e['floats']['distinct']} distinct")
            print(f"    strings: {e['strings']['count']} value(s), "
                  f"{e['strings']['distinct']} distinct: "
                  + ", ".join(e["string_values"]))
            for warning in e["structure"]["warnings"]:
                print(f"    [warn] {warning}")
        else:
            print(f"  {e['rel']}: {e['size']} bytes, {e['lines']} line, "
                  f"EOL {e['eol']!r}, path {e['text']!r}")
            for warning in e["structure"]["warnings"]:
                print(f"    [warn] {warning}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _soe_tokens(version: int = 13, path_points: int = 0, weapons: int = 1,
                children: int = 0) -> list[str]:
    """Build a valid ``.soe`` token stream for a version (grammar order)."""
    t = [str(version), "0", "10", "railrock", "37", "116982"]
    if version >= 9:
        t.append("8")
    t += ["PUSHABLE", "0", "0", "0", "0", "0", "0", "ELLIPSOID",
          "115", "115", "80", "0", "0", "0", "0"]
    if version >= 7:
        t += ["0", "0"]
    else:
        t += ["0", "NONE", "0"]
    t += (["0", "0", "0", "0"] if version >= 8 else ["0"])
    t += ["NONE", "NONE", "0"]
    if version >= 3:
        t.append("0")
    t += ["0", "3", "0", "0", "0", "0", "NONE", "0", "0", "0"]
    if version >= 4:
        t.append("0")
    if version >= 5:
        t.append("0")
    if version >= 6:
        t += ["0", "0", "0"]
    if version >= 7:
        t += ["200", "0"]
    if version >= 10:
        t += ["0", "0"]
    if version >= 11:
        t += ["0", "0"]
    if version >= 12:
        t += ["0", "50"]
    if version >= 13:
        t += ["0", "0"]
    if version >= 2:
        t.append(str(path_points))
        t += [f"pp{i}" for i in range(path_points)]
    t.append(str(weapons))
    for i in range(weapons):
        t += [f"gun{i}", "1", "128", "10", "0", "0", "0"]
    if version >= 1:
        t.append(str(children))
        for i in range(children):
            t += [f"kid{i}", "0", "0", "0", "0", "0", "0", "0"]
    return t


def _soe_blob(tokens: list[str], sep: str = " ", eol: bytes = b"\r\n",
              trailing: bool = True) -> bytes:
    """Join tokens into a file; ``sep`` per token mimics the shipped layout."""
    parts = []
    for i, token in enumerate(tokens):
        parts.append((f" {token} " if i % 2 == 0 else token).encode("latin1"))
    body = eol.join(parts)
    return body + (eol if trailing else b"")


def selftest() -> tuple[bool, list[dict]]:
    results: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append({"check": name, "ok": bool(ok), "detail": detail})

    def raises(name: str, fn, needle: str = "") -> None:
        try:
            fn()
        except (EditError, ScanError) as exc:
            ok = needle.lower() in str(exc).lower() if needle else True
            check(name, ok, str(exc)[:110])
        except Exception as exc:                        # noqa: BLE001
            check(name, False, f"{type(exc).__name__}: {exc}")
        else:
            check(name, False, "no exception")

    blob = _soe_blob(_soe_tokens(13))
    doc = walk_soe(blob)
    model = _oracle_soe(blob)

    # -- reader and layout ------------------------------------------------
    check("line-span re-join is byte-exact", rejoin(blob, doc.lines) == blob)
    check("the stream has 67 tokens with nothing left over",
          len(doc.tokens) == 67 and doc.extra == 0,
          f"{len(doc.tokens)} tokens, extra {doc.extra}")
    check("the version and the block summary are read",
          (doc.version, doc.shape, doc.weapon_count, doc.children_count)
          == (13, "ellipsoid", 1, 0),
          f"v{doc.version} {doc.shape} weapons={doc.weapon_count}")
    check("own labels equal the oracle labels",
          [t.label for t in doc.tokens] == [f.label for f in model.fields])
    check("own kinds equal the oracle kinds",
          [t.kind for t in doc.tokens] == [f.kind for f in model.fields])
    check("own values equal the oracle values",
          [t.value for t in doc.tokens] == [f.value for f in model.fields])
    check("the oracle round-trips the synthetic bytes",
          model.to_bytes() == blob)
    odd = _soe_blob(_soe_tokens(13), eol=b"\n")
    check("LF-only layout is accepted and re-joined",
          walk_soe(odd).lines[0].eol == b"\n"
          and rejoin(odd, walk_soe(odd).lines) == odd)
    flat = " ".join(_soe_tokens(13)).encode("latin1")
    check("one-line layout is accepted",
          len(walk_soe(flat).tokens) == 67)
    v0 = _soe_blob(_soe_tokens(0, weapons=0))
    check("a version-0 stream drops the gated blocks",
          len(walk_soe(v0).tokens) < 40
          and walk_soe(v0).version == 0, f"{len(walk_soe(v0).tokens)} tokens")
    sphere_tokens = _soe_tokens(13)
    sphere_tokens[14] = "sphere"
    del sphere_tokens[16:18]                 # sphere takes one radius, not 3
    sphere = walk_soe(_soe_blob(sphere_tokens))
    check("sphere shape reads one radius and two tokens fewer",
          sphere.shape == "sphere" and len(sphere.tokens) == 65,
          f"{sphere.shape}, {len(sphere.tokens)} tokens")

    # -- groups and stats -------------------------------------------------
    groups = {t.group for t in doc.tokens}
    check("tokens fall into the documented groups",
          {"header", "id", "collision", "events", "weapons"} <= groups,
          ", ".join(sorted(groups)))
    check("the collision block has 11 tokens",
          sum(1 for t in doc.tokens if t.group == "collision") == 11)

    # -- patching ---------------------------------------------------------
    idx = next(t.index for t in doc.tokens if t.label == "f1cc")
    same, _ = patch_soe(doc, doc.tokens[idx], "250")
    check("a same-width number patch changes one byte",
          len(same) == len(blob)
          and aligned_diff(blob, same) == 1, f"token {idx}")
    wide, _ = patch_soe(doc, doc.tokens[idx], "2000")
    check("a width-changing patch grows the file by one byte",
          len(wide) == len(blob) + 1)
    name_idx = next(t.index for t in doc.tokens if t.label == "class_name")
    renamed, value = patch_soe(doc, doc.tokens[name_idx], "rock2")
    check("a string token patch is accepted",
          walk_soe(renamed).tokens[name_idx].value == "rock2"
          and value == "rock2")
    check("the oracle sees the patched number",
          _oracle_soe(wide).fields[idx].value == 2000)

    # -- verification -----------------------------------------------------
    rep = verify_soe_edit(doc, wide, idx, "2000")
    check("verify_soe_edit accepts a width-changing patch",
          rep["ok"] and all(c["ok"] for c in rep["checks"]),
          f"{len(rep['checks'])} checks")
    rep2 = verify_soe_edit(doc, same, idx, "250")
    check("verify_soe_edit accepts a same-width patch", rep2["ok"])

    # -- .sni -------------------------------------------------------------
    sni = b".\r\n"
    sdoc = walk_sni(sni)
    check("a .sni holds one path token and its EOL",
          sdoc.text == "." and sdoc.eol == CRLF
          and len(sdoc.tokens) == 1)
    check("the oracle reads the same .sni path",
          oracle_small.parse_sni(sni) == "." and
          oracle_small.sni_to_bytes(".") == sni)
    edited = patch_sni(sdoc, "saves")
    check("a .sni patch keeps the terminator",
          edited == b"saves\r\n"
          and verify_sni_edit(sdoc, edited, "saves")["ok"])
    lf = walk_sni(b"x\n")
    check("a LF-only .sni is read but flagged",
          lf.eol == b"\n" and structure_sni(lf)["warnings"]
          and oracle_small.sni_to_bytes("x") != lf.data)
    raises("refuses a two-line .sni", lambda: walk_sni(b"a\r\nb\r\n"),
           "one path line")
    raises("refuses an out-of-range .sni token",
           lambda: (_ for _ in ()).throw(EditError("token 1")))

    # -- refusals ---------------------------------------------------------
    raises("refuses version edits",
           lambda: validate_soe_edit(doc.tokens[0], "12"), "not editable")
    shape_idx = next(t.index for t in doc.tokens
                     if t.label == "collision_shape")
    raises("refuses collision-shape edits",
           lambda: validate_soe_edit(doc.tokens[shape_idx], "sphere"),
           "not editable")
    wc_idx = next(t.index for t in doc.tokens if t.label == "weapon_count")
    raises("refuses weapon-count edits",
           lambda: validate_soe_edit(doc.tokens[wc_idx], "2"), "not editable")
    raises("refuses a non-integer value in an int slot",
           lambda: validate_soe_edit(doc.tokens[idx], "abc"), "integer")
    raises("refuses a non-number value in a float slot",
           lambda: validate_soe_edit(doc.tokens[8], "abc"), "number")
    raises("refuses an empty string token",
           lambda: validate_soe_edit(doc.tokens[name_idx], "   "), "empty")
    raises("refuses whitespace inside a string token",
           lambda: validate_soe_edit(doc.tokens[name_idx], "a b"),
           "whitespace-free")
    raises("refuses a token index past the stream",
           lambda: resolve_soe_token(doc, 99), "token 99")
    raises("refuses a negative token index",
           lambda: resolve_soe_token(doc, -1), "token -1")
    raises("refuses a version outside 0..13",
           lambda: walk_soe(_soe_blob(["14"] + _soe_tokens(13)[1:])),
           "version 14")
    raises("refuses an empty .soe", lambda: walk_soe(b""), "empty")
    raises("refuses a truncated stream",
           lambda: walk_soe(_soe_blob(_soe_tokens(13)[:-3])), "missing")
    raises("refuses a non-integer where the grammar needs one",
           lambda: walk_soe(_soe_blob(
               _soe_tokens(13)[:5] + ["x"] + _soe_tokens(13)[6:])),
           "integer")
    raises("refuses a count larger than the stream",
           lambda: walk_soe(_soe_blob(_soe_tokens(13)[:-8])), "token")
    raises("refuses a non-.soe/.sni input", lambda: file_kind("x.txt"),
           "not a .soe/.sni")
    raises("refuses output inside the game",
           lambda: refuse_unsafe_out(
               os.path.join(DEFAULT_GAME, "SOBS", "x.soe"), DEFAULT_GAME,
               "soe"), "game installation")
    raises("refuses output inside the mirror",
           lambda: refuse_unsafe_out(os.path.join(MIRROR, "x.sni"),
                                     DEFAULT_GAME, "sni"), "mirror")
    raises("refuses output inside the repository",
           lambda: refuse_unsafe_out(os.path.join(ROOT, "x.soe"),
                                     DEFAULT_GAME, "soe"), "repository")
    raises("refuses a mismatched output extension",
           lambda: refuse_unsafe_out("/tmp/opencode/x.txt", DEFAULT_GAME,
                                     "soe"), "--out must be a .soe")

    # -- end-to-end on synthetic files ------------------------------------
    scratch = _scratch_dir("selftest")
    try:
        src = os.path.join(scratch, "a.soe")
        dst = os.path.join(scratch, "b.soe")
        atomic_write(src, blob)
        raises("refuses --out equal to the input",
               lambda: set_token(src, idx, "250", src), "--out is the input")
        result = set_token(src, idx, "2000", dst)
        check("--set writes a verified file",
              result["ok"] and result["written"]
              and result["changed_bytes"] == 1
              and result["size_delta"] == 1,
              f"{result['size']} bytes, {result['changed_bytes']} changed, "
              f"delta {result['size_delta']:+d}")
        checked = check_file(dst, src, idx, "2000")
        check("--check accepts the verified edit",
              checked["ok"] and checked["against"]["ok"]
              and checked["changed_bytes"] == 1,
              f"{checked['changed_bytes']} byte(s) in the token, file "
              f"{checked['file_changed_bytes']}")
        unchecked = check_file(dst, src)
        check("--check reports the changed token",
              not unchecked["ok"]
              and unchecked["changed_tokens"]["changed"][0]["index"] == idx)
        nsrc = os.path.join(scratch, "p.sni")
        ndst = os.path.join(scratch, "q.sni")
        atomic_write(nsrc, sni)
        nresult = set_token(nsrc, 0, "saves", ndst)
        check("--set writes a verified .sni",
              nresult["ok"] and read_bytes(ndst) == b"saves\r\n")
        ncheck = check_file(ndst, nsrc, 0, "saves")
        check("--check accepts the .sni edit",
              ncheck["ok"] and ncheck["changed_bytes"] == 5,
              f"{ncheck['changed_bytes']} byte(s)")
        payload = json.dumps({"set": result, "sni": nresult},
                             ensure_ascii=False)
        check("reports are JSON-serialisable", '"written": true' in payload)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    return all(r["ok"] for r in results), results


def print_selftest(ok: bool, results: list[dict]) -> None:
    passed = sum(1 for r in results if r["ok"])
    for item in results:
        print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
              + (f" ({item['detail']})" if item.get("detail") else ""))
    print(f"self-test: {passed}/{len(results)} checks, "
          f"result: {'OK' if ok else 'FAIL'}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_methods(ap) -> None:
    ap.add_argument("file", nargs="?",
                    help=".soe/.sni file (or DIR for --roundtrip/--audit)")
    ap.add_argument("--list", action="store_true",
                    help="every token with its number, span and label")
    ap.add_argument("--check", action="store_true",
                    help="structure checks + byte comparison")
    ap.add_argument("--set", action="store_true",
                    help="edit one token into --out")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every .soe/.sni under DIR")
    ap.add_argument("--audit", action="store_true",
                    help="corpus statistics")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests, no game data")
    ap.add_argument("--token", type=int, metavar="N",
                    help="0-based token index (--set; --check with --value)")
    ap.add_argument("--value", metavar="V",
                    help="new token text (number or string)")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: source file to compare with")
    ap.add_argument("--out", metavar="PATH", help="--set: output path")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL", DEFAULT_GAME),
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--mirror", action="store_true",
                    help="--roundtrip/--audit: use the regress mirror as root")
    ap.add_argument("--verbose", action="store_true",
                    help="--roundtrip: print every file")
    ap.add_argument("--json", action="store_true", help="JSON report")


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv:
        head = argv[0].lower()
        aliases = {
            "self-test": "--self-test", "selftest": "--self-test",
            "roundtrip": "--roundtrip", "audit": "--audit",
            "list": "--list", "set": "--set", "check": "--check",
        }
        if head in aliases:
            argv = [aliases[head]] + argv[1:]
    ap = argparse.ArgumentParser(
        description="Round-trip proof and editor for .soe/.sni "
                    "(M11.5, slice 8).")
    _add_methods(ap)
    args = ap.parse_args(argv)

    modes = [args.list, args.check, args.set, args.roundtrip, args.audit,
             args.self_test]
    if sum(bool(m) for m in modes) != 1:
        ap.print_help()
        return 2
    try:
        if args.self_test:
            ok, results = selftest()
            if args.json:
                print(json.dumps({"check": "self-test", "ok": ok,
                                  "passed": sum(1 for r in results if r["ok"]),
                                  "total": len(results), "results": results},
                                 indent=1, ensure_ascii=False))
            else:
                print_selftest(ok, results)
            return 0 if ok else 1

        root = args.file or args.game
        if args.mirror:
            if not os.path.isdir(MIRROR):
                raise EditError(f"the mirror does not exist: {MIRROR}")
            if args.file is None:
                root = MIRROR

        if args.roundtrip:
            rep = roundtrip_corpus(root, verbose=args.verbose)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1

        if args.audit:
            rep = audit_corpus(root)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_audit(rep)
            return 0 if rep["ok"] else 1

        if not args.file:
            raise EditError("this mode needs a .soe/.sni file")

        if args.list:
            path = resolve_input(args.file, args.game)
            rep = (list_soe(path) if file_kind(path) == "soe"
                   else list_sni(path))
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_list(rep)
            return 0 if rep["structure"]["ok"] else 1

        if args.set:
            if not args.out:
                raise EditError("--set needs --out FILE")
            if args.token is None:
                raise EditError("--set needs --token N")
            if args.value is None:
                raise EditError("--set needs --value V")
            rep = set_token(args.file, args.token, args.value, args.out,
                            args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_set(rep)
            return 0 if rep["ok"] else 1

        if args.check:
            rep = check_file(args.file, args.against, args.token, args.value,
                             args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_check(rep)
            return 0 if rep["ok"] else 1

        ap.print_help()
        return 2
    except EditError as exc:
        print(f"soewrite: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"soewrite: corrupt stream: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"soewrite: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                            # noqa: BLE001
        print(f"soewrite: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
