#!/usr/bin/env python3
"""Layout-preserving writer/editor for Gunmetal ``.sob`` object models
(``M11.5``, slice 2).

``gm/sob.py`` (``M1.4``, ``docs/SOB.md``) already reads every shipped model and
can write it back: an *unmodified* model through its stored ``raw`` bytes
(byte-exact), an *edited* one as the canonical variant A text.  The canonical
writer is deliberately **not** byte-exact for the shipped files: 595 of 672 put
the ``-1`` sentinel at the end of the integer line, some omit the newline after
the plane numbers, 96 use a different float spelling and ``MOCLASS.SOB`` uses a
single-space layout (``docs/SOB.md`` §2.2).

This tool adds the missing half: an edit **rewrites only the bytes of the
edited token**.  The file is a flat whitespace-separated token stream, so a
vertex, a normal, a texture name or a face flag lives in one token; replacing
that token's bytes in place leaves every other byte, every separator and the
whole original layout untouched.  Length may change by the width of the new
number (``1432`` -> ``1496`` keeps it, ``64`` -> ``1.5`` does not), but nothing
is re-laid-out and no byte is shifted beyond that token.

What is proven here
-------------------
``--roundtrip`` reads all shipped models, writes each one back (oracle
``Sob.write``, ``exact=True``) into a scratch directory, reads the bytes back
and compares them with the original; it also re-parses the canonical text
(``to_bytes``) semantically and re-joins the file from its token spans.  The
shipped corpus is **672/672 byte-identical** (4 671 847 bytes), see
``docs/SOB_WRITE.md``.

Command surface
---------------
``--list FILE``            model summary + face table.
``--get FILE --field NAME`` one field with its token spans.
``--set FILE --field NAME=VALUE --out OUT``
                           token-level edit; refuses the game directory.
``--check FILE [--against FILE] [--field NAME=VALUE]``
                           structure + byte comparison (default source: the
                           shipped file with the same basename).
``--roundtrip [DIR]``      read -> write -> compare every ``.sob`` under DIR
                           (default: the game installation).
``--audit``                corpus statistics (counts, textures, flags, spare,
                           axes, outliers).
``--self-test``            synthetic tests, no game data.
``--json``                 machine-readable report for every mode.

Editable fields::

    vertex.N            x,y,z            (3 tokens)
    vertex.N.x|y|z      one component    (1 token)
    face.N.origin       x,y,z            (3 tokens; plane numbers 0..2)
    face.N.u|v|normal   x,y,z            (3 tokens; 65536 = unit axis)
    face.N.spare        x,y,z            (3 tokens, undocumented marker)
    face.N.flags        integer          (one token, shipped range -128..127)
    face.N.texture      token text       (slot 0 = diffuse texture)
    face.N.slotK        token text       (K = 0..7 material/texture slot)

Not editable on purpose: vertex/face counts and face indices (their text
length would move the stream), textures/lightmaps as files, and the derived
geometry (no normal/radius/UV recomputation).

Exit codes: ``0`` success, ``1`` verification failed, ``2`` bad input or a
refused edit (message on stderr), ``3`` internal error.  No traceback escapes
:func:`main`.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sob as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sobwrite"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")

#: Trust-boundary caps, identical to the oracle (``gm/sob.py:302-306``).
MAX_VERTS = oracle.MAX_VERTS                      # 200 000 (largest: 957)
MAX_FACES = oracle.MAX_FACES                      # 5 000 (largest: 1 130)

#: Value bounds measured on the corpus (``--audit``); an edit outside these is
#: refused instead of writing a number no shipped file contains.
VERT_LIMIT = 1.0e9        # shipped |vertex| max 13 779.594
PLANE_LIMIT = 1.0e12      # shipped |plane number| max 1 073 610 747.5
FLAGS_MIN, FLAGS_MAX = -128, 127   # shipped range: 96 distinct values

TEX_SLOTS = 8
PLANE_NUMBERS = 15

FIELD_RE = re.compile(r"^(vertex|face)\.(\d+)(?:\.(.+))?$")
SLOT_RE = re.compile(r"^slot([0-7])$")


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The bytes do not walk as a ``.sob`` (own reader)."""


# ---------------------------------------------------------------------------
# token stream (byte spans)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Token:
    """One whitespace-separated token and its byte span in the file."""

    text: str
    start: int
    end: int

    @property
    def span(self) -> tuple[int, int]:
        return (self.start, self.end)


def tokenize(data: bytes) -> list[Token]:
    """Split ``data`` the way ``str.split()`` does, keeping byte spans.

    ``gm/sob.py`` decodes latin-1 and calls ``str.split()``; latin-1 maps one
    byte to one character, so a manual scan with ``str.isspace()`` reproduces
    that tokenisation exactly (including the exotic separators ``\\x85`` /
    ``\\xa0``, which ``str.split()`` also consumes).
    """
    text = data.decode("latin1")
    out: list[Token] = []
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        start = i
        while i < n and not text[i].isspace():
            i += 1
        out.append(Token(text[start:i], start, i))
    return out


def rejoin(data: bytes, tokens: list[Token]) -> bytes:
    """Reassemble the file from its tokens; a proof the spans cover it."""
    parts: list[bytes] = []
    pos = 0
    for token in tokens:
        if token.start < pos or token.end < token.start or token.end > len(data):
            raise ScanError(f"token spans are out of order at {token.start}")
        parts.append(data[pos:token.start])
        parts.append(token.text.encode("latin1"))
        pos = token.end
    parts.append(data[pos:])
    return b"".join(parts)


# ---------------------------------------------------------------------------
# own reader (independent of gm/sob.py, same grammar)
# ---------------------------------------------------------------------------

@dataclass
class FaceLoc:
    """One face record: token spans plus decoded values."""

    index: int
    tex: list[Token]
    flags_tok: Token
    nverts_tok: Token
    index_toks: list[Token]
    plane_toks: list[Token]
    extra_tok: Token | None
    start: int
    end: int

    @property
    def flags(self) -> int:
        return int(self.flags_tok.text)

    @property
    def indices(self) -> list[int]:
        return [int(t.text) for t in self.index_toks]

    @property
    def planes(self) -> list[float]:
        return [float(t.text) for t in self.plane_toks]

    @property
    def origin(self) -> tuple[float, float, float]:
        p = self.planes
        return (p[0], p[1], p[2])

    @property
    def u(self) -> tuple[float, float, float]:
        p = self.planes
        return (p[3], p[4], p[5])

    @property
    def v(self) -> tuple[float, float, float]:
        p = self.planes
        return (p[6], p[7], p[8])

    @property
    def normal(self) -> tuple[float, float, float]:
        p = self.planes
        return (p[9], p[10], p[11])

    @property
    def spare(self) -> tuple[float, float, float]:
        p = self.planes
        return (p[12], p[13], p[14])

    @property
    def textures(self) -> list[str]:
        return [t.text for t in self.tex]


@dataclass
class SobLoc:
    """A walked ``.sob``: counts, vertex tokens, face spans and the header."""

    path: str
    data: bytes
    tokens: list[Token]
    count_toks: list[Token]
    vertex_toks: list[list[Token]]
    faces: list[FaceLoc]
    end_token: int

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def nverts(self) -> int:
        return len(self.vertex_toks)

    @property
    def nfaces(self) -> int:
        return len(self.faces)

    @property
    def vertices(self) -> list[tuple[float, float, float]]:
        return [tuple(float(t.text) for t in trio)    # type: ignore[misc]
                for trio in self.vertex_toks]

    @property
    def bbox(self) -> tuple[tuple[float, float, float],
                            tuple[float, float, float]]:
        verts = self.vertices
        if not verts:
            return ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        return (tuple(min(v[i] for v in verts) for i in range(3)),   # type: ignore[return-value]
                tuple(max(v[i] for v in verts) for i in range(3)))   # type: ignore[return-value]


def walk(data: bytes, path: str = "") -> SobLoc:
    """Walk the token stream, keeping every token's byte span.

    The grammar is exactly the one in ``gm/sob.py`` / ``docs/FORMATS.md`` §3:
    counts, ``3 * nverts`` vertex tokens, then per face eight texture tokens,
    ``flags nverts i0..i(nverts-1)``, the optional lone ``-1`` and fifteen
    plane numbers.  The optional sentinel makes the parse ambiguous locally,
    so it backtracks with a memo over the token position, like the oracle.
    Unlike the oracle this walker also records *where* each token is.
    """
    tokens = tokenize(data)
    if len(tokens) < 2:
        raise ScanError("fewer than two numbers")
    try:
        nverts = int(tokens[0].text)
        nfaces = int(tokens[1].text)
    except ValueError as exc:
        raise ScanError("bad vertex/face count") from exc
    if not 0 <= nverts <= MAX_VERTS or not 0 <= nfaces <= MAX_FACES:
        raise ScanError(f"implausible counts {nverts} vertices / "
                        f"{nfaces} faces")
    if 2 + 3 * nverts > len(tokens):
        raise ScanError("truncated vertex list")
    if 2 + 3 * nverts + nfaces * 28 > len(tokens) + 27:
        raise ScanError(f"face count {nfaces} does not fit the stream")
    if sys.getrecursionlimit() < nfaces * 4 + 200:
        sys.setrecursionlimit(nfaces * 4 + 200)

    pos = 2
    vertex_toks: list[list[Token]] = []
    for _ in range(nverts):
        trio = tokens[pos:pos + 3]
        for tok in trio:
            try:
                value = float(tok.text)
            except ValueError as exc:
                raise ScanError(f"bad vertex number {tok.text!r}") from exc
            if not math.isfinite(value):
                raise ScanError(f"non-finite vertex number {tok.text!r}")
        vertex_toks.append(list(trio))
        pos += 3

    memo: dict[int, int | None] = {}
    extra_of: dict[int, int] = {}

    def walk_face(p: int) -> int | None:
        if p in memo:
            return memo[p]
        memo[p] = None                              # re-entry guard
        if p == len(tokens):
            memo[p] = p
            return p
        try:
            if len(tokens) - p < TEX_SLOTS:
                return None
            q = p + TEX_SLOTS
            if q + 2 > len(tokens):
                return None
            int(tokens[q].text)                     # flags
            nv = int(tokens[q + 1].text)
            if nv < 3 or nv > 32:
                return None
            for k in range(2, 2 + nv):
                index = int(tokens[q + k].text)
                if not 0 <= index < nverts:
                    return None
            q += 2 + nv
            for k in range(PLANE_NUMBERS):
                if not math.isfinite(float(tokens[q + k].text)):
                    return None
            q += PLANE_NUMBERS
        except (ValueError, IndexError):
            return None
        for extra in (0, 1):
            r = q + extra
            if extra and (r > len(tokens) or tokens[q].text != "-1"):
                continue
            res = walk_face(r)
            if res is not None:
                memo[p] = res
                extra_of[p] = extra
                return res
        return None

    end = walk_face(pos)
    if end is None:
        raise ScanError(f"no valid parse from token {pos}")

    faces: list[FaceLoc] = []
    p = pos
    while p < end:
        tex = list(tokens[p:p + TEX_SLOTS])
        q = p + TEX_SLOTS
        flags_tok = tokens[q]
        nverts_tok = tokens[q + 1]
        nv = int(nverts_tok.text)
        index_toks = list(tokens[q + 2:q + 2 + nv])
        q += 2 + nv
        plane_toks = list(tokens[q:q + PLANE_NUMBERS])
        q += PLANE_NUMBERS
        extra_tok = None
        if extra_of.get(p):
            extra_tok = tokens[q]
            q += 1
        faces.append(FaceLoc(len(faces), tex, flags_tok, nverts_tok,
                             index_toks, plane_toks, extra_tok,
                             tokens[p].start, tokens[q - 1].end))
        p = q

    if p != end:
        raise ScanError(f"walker stopped at token {p}, expected {end}")
    if end != len(tokens):
        raise ScanError(f"{len(tokens) - end} trailing token(s)")

    return SobLoc(path, data, tokens, [tokens[0], tokens[1]], vertex_toks,
                  faces, end)


# ---------------------------------------------------------------------------
# field values
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FieldSpec:
    """What a field name addresses: token count and kind."""

    name: str
    kind: str            # "vec3" | "float" | "int" | "text"
    tokens: tuple[Token, ...]
    doc: str


def _field_alias(name: str) -> str:
    return {"tex": "texture"}.get(name, name)


def resolve_field(loc: SobLoc, name: str) -> FieldSpec:
    """``vertex.N[.axis]`` / ``face.N.<part>`` -> tokens and kind."""
    text = name.strip()
    m = FIELD_RE.match(text)
    if not m:
        raise EditError(f"bad field name {text!r}: expected vertex.N[.x|y|z] "
                        f"or face.N.<origin|u|v|normal|spare|flags|texture|"
                        f"slot0..slot7>")
    kind, index_text, part = m.group(1), m.group(2), m.group(3)
    index = int(index_text)
    if kind == "vertex":
        if index >= loc.nverts:
            raise EditError(f"vertex.{index}: the model has {loc.nverts} "
                            f"vertices (0..{loc.nverts - 1})")
        trio = loc.vertex_toks[index]
        if part is None:
            return FieldSpec(f"vertex.{index}", "vec3", tuple(trio),
                             "world-space vertex (3 float tokens, "
                             "docs/FORMATS.md §3)")
        axis = {"x": 0, "y": 1, "z": 2}.get(part)
        if axis is None:
            raise EditError(f"vertex.{index}.{part}: axis must be x, y or z")
        return FieldSpec(f"vertex.{index}.{part}", "float",
                         (trio[axis],), "one vertex component")
    if index >= loc.nfaces:
        raise EditError(f"face.{index}: the model has {loc.nfaces} faces "
                        f"(0..{loc.nfaces - 1})")
    face = loc.faces[index]
    part = _field_alias(part or "")
    if part in ("origin", "u", "v", "normal", "spare"):
        offset = {"origin": 0, "u": 3, "v": 6, "normal": 9, "spare": 12}[part]
        doc = {
            "origin": "plane origin (3 float tokens; UV = dot(p - origin, "
                      "axis)/65536)",
            "u": "U axis scaled by 65536 (16.16 planar projection)",
            "v": "V axis scaled by 65536",
            "normal": "N vector (undocumented scale; shipped |N| is not "
                      "always 65536)",
            "spare": "spare triple; shipped marker (0, 12345678|16539982|…, 0)"
                     " -- undocumented",
        }[part]
        return FieldSpec(f"face.{index}.{part}", "vec3",
                         tuple(face.plane_toks[offset:offset + 3]), doc)
    if part == "flags":
        return FieldSpec(f"face.{index}.flags", "int", (face.flags_tok,),
                         "face flag word (one integer token; semantics "
                         "undocumented, shipped range -128..127)")
    if part == "texture":
        return FieldSpec(f"face.{index}.texture", "text", (face.tex[0],),
                         "diffuse texture token (slot 0, docs/FORMATS.md §3)")
    slot = SLOT_RE.match(part)
    if slot:
        k = int(slot.group(1))
        return FieldSpec(f"face.{index}.slot{k}", "text", (face.tex[k],),
                         f"material/texture slot {k} (documented codes: none, "
                         f"translucent, add)")
    raise EditError(f"face.{index}.{part}: unknown part (origin, u, v, "
                    f"normal, spare, flags, texture, slot0..slot7)")


def oracle_value(model, spec: FieldSpec):
    """The same field as seen by the ``gm.sob`` oracle."""
    m = re.match(r"^vertex\.(\d+)(?:\.([xyz]))?$", spec.name)
    if m:
        vertex = model.vertices[int(m.group(1))]
        axis = m.group(2)
        return vertex if axis is None else vertex["xyz".index(axis)]
    m = re.match(r"^face\.(\d+)\.(.+)$", spec.name)
    if not m:
        raise EditError(f"bad field name {spec.name!r}")
    face = model.faces[int(m.group(1))]
    part = m.group(2)
    if part in ("origin", "u", "v", "normal", "spare"):
        return getattr(face, part)
    if part == "flags":
        return face.flags
    if part == "texture":
        return face.textures[0]
    slot = SLOT_RE.match(part)
    if slot:
        return face.textures[int(slot.group(1))]
    raise EditError(f"unknown field {spec.name!r}")


def fmt_number(value: int | float) -> str:
    """One ``.sob`` token: integers bare, floats shortest round-trip.

    Byte-for-byte the rule of ``gm/sob.py:_fmt_number`` minus the ``"% d "``
    padding: the padding belongs to the *separators*, which this tool keeps.
    """
    if isinstance(value, int):
        return str(value)
    f = float(value)
    if not math.isfinite(f):
        raise EditError(f"non-finite number {f!r}")
    if f == 0.0:
        return "0"
    if f.is_integer() and abs(f) < 1e16:
        return str(int(f))
    return repr(f)


def parse_float(text: str, what: str) -> float:
    try:
        value = float(text.strip())
    except ValueError:
        raise EditError(f"{what}: not a number: {text!r}") from None
    if not math.isfinite(value):
        raise EditError(f"{what}: not a finite number: {text!r}")
    return value


def parse_int(text: str, what: str) -> int:
    try:
        return int(text.strip(), 0)
    except ValueError:
        raise EditError(f"{what}: not an integer: {text!r}") from None


def parse_assignment(spec: FieldSpec, assignment: str) -> object:
    """Parse the VALUE part of ``--field NAME=VALUE`` for this field."""
    if "=" not in assignment:
        raise EditError("--field needs NAME=VALUE")
    _, _, value_text = assignment.partition("=")
    if spec.kind == "vec3":
        parts = value_text.split(",")
        if len(parts) != 3:
            raise EditError(f"{spec.name} needs three comma-separated "
                            f"numbers (got {len(parts)}: {value_text!r})")
        return tuple(parse_float(p, spec.name) for p in parts)
    if spec.kind == "float":
        return parse_float(value_text, spec.name)
    if spec.kind == "int":
        return parse_int(value_text, spec.name)
    text = value_text.strip()
    if not text:
        raise EditError(f"{spec.name}: empty texture token")
    if any(c.isspace() for c in text):
        raise EditError(f"{spec.name}: a token cannot contain whitespace: "
                        f"{text!r}")
    try:
        text.encode("latin1")
    except UnicodeEncodeError:
        raise EditError(f"{spec.name}: {text!r} is not representable in "
                        f"latin-1 (.sob is a DOS text file)") from None
    if any(not (0x20 <= ord(c) < 0x7F) for c in text):
        raise EditError(f"{spec.name}: {text!r} has non-printable characters "
                        f"(shipped texture tokens are plain ASCII)")
    return text


def validate(spec: FieldSpec, value) -> None:
    """Check a value against the range measured on the corpus."""
    if spec.kind == "vec3":
        limit = VERT_LIMIT if spec.name.startswith("vertex.") else PLANE_LIMIT
        for component in value:                    # type: ignore[union-attr]
            if not math.isfinite(component):
                raise EditError(f"{spec.name}: non-finite component "
                                f"{component!r}")
            if abs(component) > limit:
                raise EditError(
                    f"{spec.name}: |{component!r}| is outside the measured "
                    f"range (|v| <= {limit:g}; see --audit) -- refusing to "
                    f"write a number no shipped model contains")
        return
    if spec.kind == "float":
        limit = VERT_LIMIT if spec.name.startswith("vertex.") else PLANE_LIMIT
        if abs(value) > limit:
            raise EditError(f"{spec.name}: |{value!r}| is outside the "
                            f"measured range (|v| <= {limit:g})")
        return
    if spec.kind == "int":
        if not FLAGS_MIN <= value <= FLAGS_MAX:
            raise EditError(f"{spec.name}={value}: face flags are a byte read "
                            f"by int() in the engine; shipped range "
                            f"{FLAGS_MIN}..{FLAGS_MAX} (96 distinct values)")
        return


def render_tokens(spec: FieldSpec, value) -> list[str]:
    """Token texts ``str``-formatted for the splice."""
    if spec.kind == "vec3":
        return [fmt_number(v) for v in value]      # type: ignore[union-attr]
    if spec.kind in ("float", "int"):
        return [fmt_number(value)]                 # type: ignore[arg-type]
    return [str(value)]


def current_value(loc: SobLoc, spec: FieldSpec):
    """Read the field from our own walker."""
    if spec.kind == "text":
        return spec.tokens[0].text
    values = [float(t.text) for t in spec.tokens]
    if spec.kind == "int":
        return int(spec.tokens[0].text)
    if spec.kind == "float":
        return values[0]
    return tuple(values)


# ---------------------------------------------------------------------------
# patching
# ---------------------------------------------------------------------------

def splice(data: bytes, replacements: list[tuple[Token, str]]) -> bytes:
    """Replace only the given tokens; every other byte stays where it was."""
    ordered = sorted(replacements, key=lambda item: item[0].start)
    parts: list[bytes] = []
    pos = 0
    for token, text in ordered:
        if token.start < pos:
            raise EditError("overlapping token spans")
        parts.append(data[pos:token.start])
        parts.append(text.encode("latin1"))
        pos = token.end
    parts.append(data[pos:])
    return b"".join(parts)


def patch(loc: SobLoc, spec: FieldSpec, value) -> tuple[bytes, list[str]]:
    """Return the edited bytes and the token texts that were written."""
    texts = render_tokens(spec, value)
    if len(texts) != len(spec.tokens):
        raise EditError(f"{spec.name}: internal token count mismatch")
    return splice(loc.data, list(zip(spec.tokens, texts))), texts


def aligned_diff(old: bytes, new: bytes) -> int:
    """Bytes that differ when the two texts are aligned from the start."""
    compared = sum(1 for a, b in zip(old, new) if a != b)
    return compared + abs(len(old) - len(new))


def changed_bytes(loc: SobLoc, spec: FieldSpec, new_bytes: bytes) -> dict:
    """Describe the edit: which token spans changed and by how many bytes.

    The edited file is re-walked, so the new text of every target token comes
    from the file itself (not from the value we meant to write) -- a formatter
    mistake would show up as a diff against the value check in
    :func:`verify_edit`, not be hidden here.
    """
    new_loc = walk(new_bytes, loc.path)
    old_tokens = [t.text for t in loc.tokens]
    new_tokens = [t.text for t in new_loc.tokens]
    if len(old_tokens) != len(new_tokens):
        raise EditError("the edited file changed the token count")
    target_ids = [t.start for t in spec.tokens]
    per_token: list[dict] = []
    total = 0
    for token in spec.tokens:
        position = next(i for i, candidate in enumerate(loc.tokens)
                        if candidate.start == token.start)
        new_text = new_tokens[position]
        count = aligned_diff(token.text.encode("latin1"),
                             new_text.encode("latin1"))
        total += count
        per_token.append({"old": token.text, "new": new_text,
                          "span": list(token.span), "changed": count})
    for position, (old_text, new_text) in enumerate(zip(old_tokens, new_tokens)):
        if old_text != new_text and position not in [
                next(i for i, candidate in enumerate(loc.tokens)
                     if candidate.start == start) for start in target_ids]:
            raise EditError(f"token {position} changed unexpectedly "
                            f"({old_text!r} -> {new_text!r})")
    return {"size": len(new_bytes), "size_delta": len(new_bytes) - len(loc.data),
            "tokens": per_token, "changed_bytes": total}


# ---------------------------------------------------------------------------
# paths and refusal rules
# ---------------------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def write_bytes(path: str, data: bytes) -> None:
    try:
        oracle.atomic_write(path, data)
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def require_sob(path: str) -> str:
    """Input files must be ``.sob`` -- everything else is a mistyped call."""
    if not path.lower().endswith(".sob"):
        raise EditError(f"not a .sob file: {path}")
    if not os.path.isfile(path):
        raise EditError(f"no such file: {path}")
    return path


def _real(path: str) -> str:
    return os.path.realpath(os.path.abspath(path))


def game_roots(game: str) -> list[str]:
    roots = []
    for candidate in (game, DEFAULT_GAME, MIRROR):
        if candidate:
            roots.append(_real(candidate))
    return sorted(set(roots))


def refuse_inside_game(out_path: str, game: str) -> None:
    """Never write into the installation or the mirror used by ``regress``."""
    target = _real(out_path)
    for root in game_roots(game):
        if target == root or target.startswith(root + os.sep):
            raise EditError(
                f"refusing to write inside the game installation: {out_path} "
                f"(the game is read-only; write to /tmp/opencode or /var/tmp)")


def resolve_input(name: str, game: str = DEFAULT_GAME) -> str:
    """``tick`` / ``tick.sob`` / an explicit path -> an existing ``.sob``."""
    if os.path.isfile(name):
        return require_sob(name)
    base = name if name.lower().endswith(".sob") else name + ".sob"
    for folder in (os.path.join(game, "SOBS"), game):
        direct = os.path.join(folder, base)
        if os.path.isfile(direct):
            return direct
    for folder in (os.path.join(game, "SOBS"), game):
        try:
            entries = os.listdir(folder)
        except OSError:
            continue
        low = base.lower()
        for entry in entries:
            if entry.lower() == low:
                return os.path.join(folder, entry)
    raise EditError(f"model not found: {name} (looked in {game})")


def corpus_files(root: str) -> list[str]:
    """Every ``.sob`` under ``root`` (recursive; the shipped set is flat)."""
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames
                             if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower().endswith(".sob"):
                found.append(os.path.join(dirpath, filename))
    return found


def shipped_same_name(path: str, game: str) -> str | None:
    """The shipped file with the same basename, if there is one."""
    base = os.path.basename(path)
    for folder in (os.path.join(game, "SOBS"), game):
        candidate = os.path.join(folder, base)
        if os.path.isfile(candidate):
            return candidate
    return None


def default_against(path: str, game: str) -> str | None:
    """The shipped file with the same basename, unless it *is* this file."""
    candidate = shipped_same_name(path, game)
    if candidate is None or _real(candidate) == _real(path):
        return None
    return candidate


# ---------------------------------------------------------------------------
# structure checks
# ---------------------------------------------------------------------------

def oracle_parse(data: bytes, name: str = ""):
    return oracle.parse_data(data, name=name)


def structure_report(loc: SobLoc, model) -> dict:
    """Structural checks of one file: own walker vs oracle vs header."""
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    check("own walker reaches EOF", loc.end_token == len(loc.tokens),
          f"{loc.end_token}/{len(loc.tokens)} tokens")
    check("token spans re-join the file", rejoin(loc.data, loc.tokens) == loc.data)
    check("header counts match the walk", loc.nverts == int(loc.count_toks[0].text)
          and loc.nfaces == int(loc.count_toks[1].text),
          f"{loc.nverts} vertices, {loc.nfaces} faces")
    check("oracle counts match the walk",
          len(model.vertices) == loc.nverts and len(model.faces) == loc.nfaces)
    check("oracle vertices equal the walk", model.vertices == loc.vertices)
    ok = len(model.faces) == loc.nfaces
    if ok:
        for face, golden in zip(loc.faces, model.faces):
            if (face.textures != golden.textures
                    or face.flags != golden.flags
                    or face.indices != golden.indices
                    or face.origin != golden.origin
                    or face.u != golden.u or face.v != golden.v
                    or face.normal != golden.normal
                    or face.spare != golden.spare):
                ok = False
                break
    check("oracle faces equal the walk", ok)
    semantic = False
    exact = False
    try:
        encoded = model.to_bytes()
        semantic = model.content_equal(oracle_parse(encoded, model.name))
        expected = model.raw if model.is_unmodified() else encoded
        exact = expected == loc.data
    except (ValueError, OSError):
        pass
    check("oracle semantic re-parse (to_bytes -> parse)", semantic)
    check("oracle exact writer reproduces the file", exact)
    sentinels = sum(1 for face in loc.faces if face.extra_tok is not None)
    check("optional lone -1 sentinel is accounted for", True,
          f"{sentinels} face(s) use it")
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "sentinels": sentinels}


def verify_edit(loc: SobLoc, new_bytes: bytes, spec: FieldSpec,
                expected) -> dict:
    """Prove the edit changed the target token(s) and nothing else."""
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        new_loc = walk(new_bytes, loc.path)
        check("own walker parses the edited file", True)
    except ScanError as exc:
        check("own walker parses the edited file", False, str(exc))
        return {"ok": False, "checks": checks}
    try:
        new_model = oracle_parse(new_bytes, spec.name)
        check("oracle parses the edited file to EOF", True)
    except ValueError as exc:
        check("oracle parses the edited file to EOF", False, str(exc))
        return {"ok": False, "checks": checks}

    value_changed = not expected_equal(loc, spec, expected)
    delta = changed_bytes(loc, spec, new_bytes)
    check("size change equals the token width change",
          delta["size_delta"] == sum(
              len(item["new"].encode("latin1"))
              - len(item["old"].encode("latin1")) for item in delta["tokens"]),
          f"{len(loc.data)} -> {len(new_bytes)} bytes "
          f"({delta['size_delta']:+d})")
    check("token count is unchanged", len(new_loc.tokens) == len(loc.tokens),
          f"{len(loc.tokens)} tokens")

    old_texts = [t.text for t in loc.tokens]
    new_texts = [t.text for t in new_loc.tokens]
    target_positions = {i for i, tok in enumerate(loc.tokens)
                        if tok.start in {t.start for t in spec.tokens}}
    diff_positions = {i for i, (a, b) in enumerate(zip(old_texts, new_texts))
                      if a != b}
    check("every other token is identical",
          diff_positions <= target_positions,
          f"{len(diff_positions - target_positions)} unexpected change(s)")

    # Non-target bytes: compare the segments between the target spans.
    new_spec = resolve_field(new_loc, spec.name)
    old_spans = sorted(t.span for t in spec.tokens)
    new_spans = sorted(t.span for t in new_spec.tokens)
    old_parts: list[bytes] = []
    new_parts: list[bytes] = []
    pos_old = pos_new = 0
    for (a0, a1), (b0, b1) in zip(old_spans, new_spans):
        old_parts.append(loc.data[pos_old:a0])
        new_parts.append(new_bytes[pos_new:b0])
        pos_old, pos_new = a1, b1
    old_parts.append(loc.data[pos_old:])
    new_parts.append(new_bytes[pos_new:])
    check("bytes outside the target token(s) are identical",
          old_parts == new_parts)

    check("own walker sees the new value", values_equal(
        current_value(new_loc, new_spec), expected_link(spec, expected)),
        f"{show_value(current_value(new_loc, new_spec))}")
    check("oracle sees the new value", values_equal(
        oracle_value(new_model, spec), expected_link(spec, expected)),
        f"{show_value(oracle_value(new_model, spec))}")
    check("changed-byte count is reported", True,
          f"{delta['changed_bytes']} byte(s)"
          + (" (no-op)" if not value_changed
             and delta["changed_bytes"] == 0 else ""))
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "diff": delta, "new": new_loc, "new_model": new_model}


def expected_link(spec: FieldSpec, expected):
    """The comparison value for a field (ints and floats both work)."""
    if spec.kind == "vec3":
        return tuple(float(v) for v in expected)   # type: ignore[arg-type]
    if spec.kind == "float":
        return float(expected)                     # type: ignore[arg-type]
    return expected


def expected_equal(loc: SobLoc, spec: FieldSpec, expected) -> bool:
    return values_equal(current_value(loc, spec), expected_link(spec, expected))


def values_equal(a, b) -> bool:
    if isinstance(a, tuple) or isinstance(b, tuple):
        try:
            left = tuple(float(x) for x in a)        # type: ignore[arg-type]
            right = tuple(float(x) for x in b)       # type: ignore[arg-type]
        except (TypeError, ValueError):
            return a == b
        return left == right
    if isinstance(a, float) or isinstance(b, float):
        try:
            return float(a) == float(b)
        except (TypeError, ValueError):
            return a == b
    return a == b


def show_value(value) -> str:
    if isinstance(value, (tuple, list)):
        return "(" + ", ".join(fmt_number(v) for v in value) + ")"
    if isinstance(value, float):
        return fmt_number(value)
    return str(value)


def diff_report(old: bytes, new: bytes,
                old_tokens: list[str] | None = None,
                new_tokens: list[str] | None = None) -> dict:
    """Byte differences (contiguous ranges) and token-level differences."""
    changed_tokens = None
    if old_tokens is not None and new_tokens is not None \
            and len(old_tokens) == len(new_tokens):
        changed_tokens = sum(1 for a, b in zip(old_tokens, new_tokens)
                             if a != b)
    if len(old) == len(new):
        offsets = [i for i, (a, b) in enumerate(zip(old, new)) if a != b]
        ranges: list[list[int]] = []
        for offset in offsets:
            if ranges and offset == ranges[-1][1]:
                ranges[-1][1] = offset + 1
            else:
                ranges.append([offset, offset + 1])
        return {"same_size": True, "changed_bytes": len(offsets),
                "changed_tokens": changed_tokens, "size_delta": 0,
                "ranges": ranges[:20], "ranges_total": len(ranges)}
    return {"same_size": False, "changed_bytes": abs(len(new) - len(old)),
            "changed_tokens": changed_tokens, "size_delta": len(new) - len(old),
            "ranges": [], "ranges_total": 0}


def diff_summary(diff: dict) -> str:
    """One line for reports: bytes when the sizes match, else token counts."""
    if diff["same_size"]:
        text = f"{diff['changed_bytes']} byte(s) differ"
    else:
        text = f"size differs by {diff['size_delta']:+d} byte(s)"
    if diff.get("changed_tokens") is not None:
        text += f", {diff['changed_tokens']} token(s) differ"
    return text


# ---------------------------------------------------------------------------
# modes: list / get / set / check
# ---------------------------------------------------------------------------

def list_file(path: str, limit: int = 8) -> dict:
    path = resolve_input(path)
    data = read_bytes(path)
    loc = walk(data, path)
    model = oracle_parse(data, os.path.basename(path))
    structure = structure_report(loc, model)
    bbox = loc.bbox
    textures = sorted({t for face in loc.faces for t in face.textures[0:1]})
    materials = sorted({t for face in loc.faces for t in face.textures[1:]})
    faces = []
    for face in loc.faces[:limit] if limit else loc.faces:
        faces.append({"index": face.index, "texture": face.textures[0],
                      "flags": face.flags, "nverts": len(face.index_toks),
                      "origin": list(face.origin),
                      "span": [face.start, face.end]})
    return {"path": path, "size": len(data), "vertices": loc.nverts,
            "faces": loc.nfaces, "bbox": [list(bbox[0]), list(bbox[1])],
            "textures": textures, "materials": materials,
            "structure": structure, "face_list": faces,
            "shown": len(faces)}


def print_list(rep: dict, limit: int = 8) -> None:
    print(f"file: {rep['path']} ({rep['size']} bytes)")
    print(f"vertices: {rep['vertices']}  faces: {rep['faces']}  "
          f"tokens: {rep['structure']['checks'][0]['detail']}")
    lo, hi = rep["bbox"]
    print("bbox: (" + ", ".join(fmt_number(v) for v in lo) + ") .. ("
          + ", ".join(fmt_number(v) for v in hi) + ")")
    print(f"textures: {len(rep['textures'])} distinct; materials: "
          f"{', '.join(rep['materials'])}")
    shown = rep["shown"]
    print(f"faces (showing {shown} of {rep['faces']}):")
    for face in rep["face_list"]:
        origin = ", ".join(fmt_number(v) for v in face["origin"])
        print(f"  face[{face['index']:4d}] {face['texture']:<12} "
              f"flags={face['flags']:<5} nverts={face['nverts']:<3} "
              f"origin=({origin}) bytes {face['span'][0]}..{face['span'][1]}")


def get_field(path: str, name: str) -> dict:
    path = resolve_input(path)
    data = read_bytes(path)
    loc = walk(data, path)
    model = oracle_parse(data, os.path.basename(path))
    spec = resolve_field(loc, name)
    ours = current_value(loc, spec)
    theirs = oracle_value(model, spec)
    return {"path": path, "size": len(data), "field": spec.name,
            "kind": spec.kind, "value": _json_value(ours),
            "oracle_value": _json_value(theirs),
            "tokens": [{"text": t.text, "span": list(t.span)}
                       for t in spec.tokens],
            "doc": spec.doc}


def _json_value(value):
    if isinstance(value, tuple):
        return list(value)
    return value


def print_get(rep: dict) -> None:
    print(f"file: {rep['path']} ({rep['size']} bytes)")
    print(f"  {rep['field']} = {show_value(tuple(rep['value']))
                                     if rep['kind'] == 'vec3'
                                     else rep['value']}")
    print(f"  {rep['doc']}")
    for token in rep["tokens"]:
        print(f"  token {token['span'][0]}..{token['span'][1]}: "
              f"{token['text']!r}")


def set_field(path: str, assignment: str, out_path: str,
              game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    refuse_inside_game(out_path, game)
    if _real(out_path) == _real(path):
        raise EditError(f"--out is the input file: {out_path} (refusing to "
                        f"edit in place; the game/source stays untouched)")
    data = read_bytes(path)
    loc = walk(data, path)
    name = assignment.split("=", 1)[0].strip() if "=" in assignment else ""
    if not name:
        raise EditError("--field needs NAME=VALUE")
    spec = resolve_field(loc, name)
    value = parse_assignment(spec, assignment)
    validate(spec, value)
    before = current_value(loc, spec)
    new_bytes, _texts = patch(loc, spec, value)
    new_loc = walk(new_bytes, path)
    new_spec = resolve_field(new_loc, spec.name)
    check_value = current_value(new_loc, new_spec)
    if not values_equal(check_value, expected_link(spec, value)):
        raise EditError(f"{spec.name}: the written token does not read back "
                        f"as {show_value(value)} (got {show_value(check_value)})")
    report = verify_edit(loc, new_bytes, spec, value)
    if not report["ok"]:
        raise EditError("edit verification failed: " + "; ".join(
            c["detail"] or c["check"] for c in report["checks"] if not c["ok"]))
    write_bytes(out_path, new_bytes)
    stored = read_bytes(out_path)
    noop = stored == data
    return {"source": path, "out": out_path, "field": spec.name,
            "kind": spec.kind, "before": _json_value(before),
            "after": _json_value(value), "noop": noop,
            "source_size": len(data), "size": len(new_bytes),
            "size_delta": len(new_bytes) - len(data),
            "changed_bytes": report["diff"]["changed_bytes"],
            "tokens": report["diff"]["tokens"], "checks": report["checks"],
            "ok": stored == new_bytes}


def print_set(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    print(f"{rep['field']}: {show_value(rep['before'])} -> "
          f"{show_value(tuple(rep['after'])) if rep['kind'] == 'vec3' else rep['after']}"
          f" ({rep['source_size']} -> {rep['size']} bytes, "
          f"delta {rep['size_delta']:+d})")
    for item in rep["tokens"]:
        if item.get("changed"):
            print(f"  token {item['span'][0]}..{item['span'][1]}: "
                  f"{item['old']!r} -> {item.get('new', '?')!r} "
                  f"({item['changed']} byte(s) changed)")
    for check in rep["checks"]:
        mark = "ok  " if check["ok"] else "FAIL"
        print(f"  [{mark}] {check['check']}"
              + (f" ({check['detail']})" if check["detail"] else ""))
    if rep["noop"]:
        print("note: the value was already present; the file is byte-identical")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


def check_file(path: str, against: str | None = None,
               field: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    path = require_sob(path)
    data = read_bytes(path)
    loc = walk(data, path)
    model = oracle_parse(data, os.path.basename(path))
    structure = structure_report(loc, model)
    rep: dict = {"path": path, "size": len(data), "structure": structure,
                 "against": None, "field": None, "ok": structure["ok"]}
    if field is not None and against is None:
        raise EditError("--field needs --against (or a shipped file with the "
                        "same basename) to compare with")
    if against is None:
        against = default_against(path, game)
        if against is not None:
            rep["note"] = f"compared against the shipped {against}"
        else:
            shipped = shipped_same_name(path, game)
            if shipped is not None:
                rep["note"] = (f"this is the shipped model itself ({shipped}); "
                               f"pass --against to compare an edited copy")
            else:
                rep["note"] = ("no --against and no shipped file with this "
                               "basename: byte comparison skipped")
    if against is not None:
        against = require_sob(against)
        old_bytes = read_bytes(against)
        old_loc = walk(old_bytes, against)
        old_model = oracle_parse(old_bytes, os.path.basename(against))
        old_structure = structure_report(old_loc, old_model)
        rep["against"] = against
        rep["against_structure"] = old_structure
        rep["ok"] = rep["ok"] and old_structure["ok"]
        checks: list[dict] = []

        def check(name: str, ok: bool, detail: str = "") -> None:
            checks.append({"check": name, "ok": bool(ok), "detail": detail})

        check("token counts match the source",
              len(loc.tokens) == len(old_loc.tokens),
              f"{len(old_loc.tokens)} vs {len(loc.tokens)}")
        check("vertex/face counts match the source",
              (loc.nverts, loc.nfaces) == (old_loc.nverts, old_loc.nfaces))
        if field is not None:
            name = field.split("=", 1)[0].strip() if "=" in field else ""
            old_spec = resolve_field(old_loc, name)
            value = parse_assignment(old_spec, field)
            validate(old_spec, value)
            expected_bytes, _ = patch(old_loc, old_spec, value)
            token_diff = diff_report(
                old_bytes, data, [t.text for t in old_loc.tokens],
                [t.text for t in loc.tokens])
            check("the file is exactly the source with the field patched",
                  data == expected_bytes, diff_summary(token_diff))
            new_spec = resolve_field(loc, name)
            check("the field carries the expected value",
                  values_equal(current_value(loc, new_spec), value),
                  show_value(current_value(loc, new_spec)))
            old_texts = [t.text for t in old_loc.tokens]
            new_texts = [t.text for t in loc.tokens]
            target_positions = {i for i, tok in enumerate(old_loc.tokens)
                                if tok.start in {t.start
                                                 for t in old_spec.tokens}}
            diff_positions = {i for i, (a, b) in enumerate(zip(old_texts,
                                                               new_texts))
                              if a != b}
            check("only the target token(s) differ",
                  diff_positions <= target_positions,
                  f"{len(diff_positions - target_positions)} unexpected "
                  f"change(s)")
            diff = token_diff
            rep["field"] = {"name": old_spec.name,
                            "value": _json_value(value),
                            "diff": diff,
                            "changed_tokens": len(diff_positions)}
        else:
            diff = diff_report(old_bytes, data,
                               [t.text for t in old_loc.tokens],
                               [t.text for t in loc.tokens])
            check("the file is byte-identical to the source",
                  data == old_bytes, diff_summary(diff))
            rep["diff"] = diff
        rep["checks"] = checks
        rep["ok"] = rep["ok"] and all(c["ok"] for c in checks)
    return rep


def print_check(rep: dict) -> None:
    print(f"check: {rep['path']} ({rep['size']} bytes)")
    for check in rep["structure"]["checks"]:
        mark = "ok  " if check["ok"] else "FAIL"
        print(f"  [{mark}] {check['check']}"
              + (f" ({check['detail']})" if check["detail"] else ""))
    if rep.get("against"):
        print(f"against: {rep['against']}")
        for check in rep.get("checks", []):
            mark = "ok  " if check["ok"] else "FAIL"
            print(f"  [{mark}] {check['check']}"
                  + (f" ({check['detail']})" if check["detail"] else ""))
        if rep.get("field"):
            field = rep["field"]
            value = field["value"]
            shown = (show_value(tuple(value)) if isinstance(value, list)
                     else show_value(value))
            print(f"field: {field['name']} = {shown}, "
                  f"{diff_summary(field['diff'])}")
    elif rep.get("note"):
        print(f"note: {rep['note']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# corpus modes
# ---------------------------------------------------------------------------

def roundtrip_corpus(root: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    if os.path.isfile(root):
        files = [require_sob(root)]
    elif os.path.isdir(root):
        files = corpus_files(root)
    else:
        raise EditError(f"no such directory: {root}")
    if not files:
        raise EditError(f"no .sob files under {root}")
    tmp_root = os.environ.get("SOBWRITE_TMP", "/tmp/opencode")
    try:
        os.makedirs(tmp_root, exist_ok=True)
    except OSError:
        tmp_root = tempfile.gettempdir()
    scratch = tempfile.mkdtemp(prefix="sobwrite-rt-", dir=tmp_root)
    identical = 0
    canonical = 0
    semantic = 0
    rejoined = 0
    walked = 0
    failures: list[dict] = []
    tokens_total = 0
    sizes_total = 0
    try:
        for number, path in enumerate(files):
            name = os.path.basename(path)
            try:
                data = read_bytes(path)
                sizes_total += len(data)
                loc = walk(data, path)
                walked += 1
                tokens_total += len(loc.tokens)
                if rejoin(data, loc.tokens) == data:
                    rejoined += 1
                model = oracle_parse(data, name)
                if model.to_bytes() == data:
                    canonical += 1
                if model.content_equal(oracle_parse(model.to_bytes(), name)):
                    semantic += 1
                target = os.path.join(scratch, f"{number:04d}-{name}")
                model.write(target, force=True, exact=True)
                stored = read_bytes(target)
                if stored == data:
                    identical += 1
                else:
                    failures.append({"file": path, "why": "write differs",
                                     "detail": diff_summary(diff_report(data, stored))})
            except (EditError, ScanError, ValueError, OSError) as exc:
                failures.append({"file": path, "why": "error", "detail": str(exc)})
            if verbose and len(failures) and failures[-1]["file"] == path:
                print(f"  FAIL {name}: {failures[-1]['detail']}",
                      file=sys.stderr)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    total = len(files)
    return {"root": root, "files": total, "identical": identical,
            "bytes": sizes_total, "walked": walked, "rejoined": rejoined,
            "canonical_exact": canonical, "semantic": semantic,
            "tokens": tokens_total, "failures": failures,
            "ok": identical == total and walked == total and rejoined == total
            and semantic == total}


def print_roundtrip(rep: dict) -> None:
    total = rep["files"]
    print(f"{rep['identical']}/{total} identical, {rep['bytes']} bytes "
          f"({rep['root']})")
    print(f"  own walker: {rep['walked']}/{total} parsed to EOF; "
          f"token-span re-join: {rep['rejoined']}/{total} "
          f"({rep['tokens']} tokens)")
    print(f"  semantic re-parse (parse -> to_bytes -> parse): "
          f"{rep['semantic']}/{total}")
    print(f"  canonical layout identical to the shipped file: "
          f"{rep['canonical_exact']}/{total} (the rest keep their layout "
          f"through raw bytes)")
    if rep["failures"]:
        print(f"  failures: {len(rep['failures'])}")
        for item in rep["failures"][:10]:
            print(f"    {item['file']}: {item['why']} {item['detail']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


def _median(values: list[int]) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def _buckets(values: list[int]) -> dict:
    edges = [(0, 16), (17, 64), (65, 256), (257, 1024), (1025, 10 ** 9)]
    out = {}
    for low, high in edges:
        out[f"{low}-{high}" if high < 10 ** 9 else f"{low}+"] = sum(
            1 for v in values if low <= v <= high)
    return out


def audit_corpus(root: str = DEFAULT_GAME) -> dict:
    files = corpus_files(root) if os.path.isdir(root) else [require_sob(root)]
    if not files:
        raise EditError(f"no .sob files under {root}")
    verts_per_model: list[int] = []
    faces_per_model: list[int] = []
    nverts_hist: dict[int, int] = {}
    flags: dict[int, int] = {}
    spare: dict[str, int] = {}
    materials: dict[str, int] = {}
    textures: set[str] = set()
    axis_exact = axis_zero = axis_other = 0
    origin_minus_one = 0
    sentinels = 0
    sparse_faces = 0
    bytes_total = 0
    vertices_total = 0
    faces_total = 0
    errors: list[str] = []
    for path in files:
        try:
            data = read_bytes(path)
            bytes_total += len(data)
            loc = walk(data, path)
            verts_per_model.append(loc.nverts)
            faces_per_model.append(loc.nfaces)
            vertices_total += loc.nverts
            faces_total += loc.nfaces
            for face in loc.faces:
                nv = len(face.index_toks)
                nverts_hist[nv] = nverts_hist.get(nv, 0) + 1
                flags[face.flags] = flags.get(face.flags, 0) + 1
                key = ",".join(fmt_number(v) for v in face.spare)
                spare[key] = spare.get(key, 0) + 1
                textures.add(face.textures[0])
                for slot in face.textures[1:]:
                    materials[slot] = materials.get(slot, 0) + 1
                if face.origin[0] == -1.0:
                    origin_minus_one += 1
                if face.extra_tok is not None:
                    sentinels += 1
                for axis in (face.u, face.v, face.normal):
                    norm = math.sqrt(sum(c * c for c in axis))
                    if abs(norm - 65536.0) < 1e-3:
                        axis_exact += 1
                    elif norm == 0.0:
                        axis_zero += 1
                    else:
                        axis_other += 1
            if not any(face.textures[1:] != ["none"] * 7
                       for face in loc.faces):
                sparse_faces += 1
        except (EditError, ScanError, ValueError, OSError) as exc:
            errors.append(f"{path}: {exc}")

    def top(mapping: dict, count: int = 6) -> list[dict]:
        items = sorted(mapping.items(), key=lambda item: -item[1])[:count]
        return [{"value": k, "faces": v} for k, v in items]

    # canonical/semantic on the same corpus, so the numbers live together
    canonical = 0
    semantic = 0
    for path in files:
        try:
            data = read_bytes(path)
            model = oracle_parse(data, os.path.basename(path))
            if model.to_bytes() == data:
                canonical += 1
            if model.content_equal(oracle_parse(model.to_bytes())):
                semantic += 1
        except (ValueError, OSError):
            pass
    return {
        "root": root, "files": len(files), "bytes": bytes_total,
        "vertices": vertices_total, "faces": faces_total,
        "vertices_per_model": {
            "min": min(verts_per_model), "max": max(verts_per_model),
            "median": _median(verts_per_model), "buckets": _buckets(verts_per_model)},
        "faces_per_model": {
            "min": min(faces_per_model), "max": max(faces_per_model),
            "median": _median(faces_per_model), "buckets": _buckets(faces_per_model)},
        "faces_by_nverts": {str(k): nverts_hist[k]
                            for k in sorted(nverts_hist)},
        "textures": {"distinct": len(textures),
                     "material_tokens": top(materials, 8)},
        "flags": {"distinct": len(flags), "min": min(flags), "max": max(flags),
                  "top": top(flags)},
        "spare": {"distinct_triples": len(spare), "top": top(spare)},
        "axes": {"unit_65536": axis_exact, "zero": axis_zero,
                 "other": axis_other,
                 "triples": 3 * faces_total},
        "undocumented": {
            "flags_faces": faces_total,
            "flags_semantics": "not documented (integer token, read by int() "
                               "in res/sob.cpp)",
            "spare_triples": len(spare),
            "lone_sentinel_after_plane": sentinels,
            "models_without_material_tokens": sparse_faces,
        },
        "lightmaps": {
            "field_in_sob": False,
            "faces_with_lightmap_index": 0,
            "note": "the .sob grammar has no lightmap index (8 texture tokens "
                    "+ flags/nverts/indices + 15 plane numbers are all "
                    "accounted for); lightmaps are per-polygon in .sbw and per "
                    "level in <level>.sbc (docs/FORMATS.md §4)",
        },
        "origin_x_minus_one_faces": origin_minus_one,
        "canonical_exact": canonical, "semantic": semantic,
        "errors": errors, "ok": not errors,
    }


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} models, {rep['bytes']} bytes, "
          f"{rep['vertices']} vertices, {rep['faces']} faces "
          f"({rep['root']})")
    v = rep["vertices_per_model"]
    f = rep["faces_per_model"]
    print(f"  vertices/model: min {v['min']}, median {v['median']:g}, "
          f"max {v['max']}; faces/model: min {f['min']}, "
          f"median {f['median']:g}, max {f['max']}")
    print("  vertices/model buckets: "
          + ", ".join(f"{k}: {n}" for k, n in v["buckets"].items()))
    print("  faces/model buckets: "
          + ", ".join(f"{k}: {n}" for k, n in f["buckets"].items()))
    print("  faces by vertex count: "
          + ", ".join(f"{k}: {n}" for k, n in rep["faces_by_nverts"].items()))
    print(f"  textures: {rep['textures']['distinct']} distinct; material "
          f"tokens: " + ", ".join(
              f"{item['value']}={item['faces']}"
              for item in rep["textures"]["material_tokens"]))
    print(f"  flags: {rep['flags']['distinct']} distinct, range "
          f"{rep['flags']['min']}..{rep['flags']['max']}; top "
          + ", ".join(f"{item['value']}={item['faces']}"
                      for item in rep["flags"]["top"]))
    print(f"  spare triples: {rep['spare']['distinct_triples']} distinct; top "
          + ", ".join(f"({item['value']})={item['faces']}"
                      for item in rep["spare"]["top"]))
    axes = rep["axes"]
    print(f"  plane axes (U/V/N, {axes['triples']} triples): "
          f"|a|=65536 {axes['unit_65536']}, zero {axes['zero']}, other "
          f"{axes['other']}")
    print(f"  lone -1 after the plane numbers: "
          f"{rep['undocumented']['lone_sentinel_after_plane']} faces; "
          f"origin.x=-1 (variant B alignment): "
          f"{rep['origin_x_minus_one_faces']} faces")
    print(f"  models with no material token: "
          f"{rep['undocumented']['models_without_material_tokens']}")
    print(f"  lightmaps: field in .sob = "
          f"{rep['lightmaps']['field_in_sob']} "
          f"({rep['lightmaps']['faces_with_lightmap_index']} faces) -- "
          f"lightmaps live in .sbw/.sbc")
    print(f"  canonical layout identical: {rep['canonical_exact']}/"
          f"{rep['files']}; semantic re-parse: {rep['semantic']}/"
          f"{rep['files']}")
    if rep["errors"]:
        print(f"  errors: {len(rep['errors'])}")
        for line in rep["errors"][:5]:
            print(f"    {line}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# self-test (no game data)
# ---------------------------------------------------------------------------

def _synth_model():
    face0 = oracle.SobFace(
        textures=["TICK", "none", "none", "none", "none", "none", "none", "none"],
        flags=96, indices=[0, 1, 2],
        origin=(0.0, 0.0, 0.0), u=(65536.0, 0.0, 0.0),
        v=(0.0, 65536.0, 0.0), normal=(0.0, 0.0, 65536.0),
        spare=(0.0, 12345678.0, 0.0))
    face1 = oracle.SobFace(
        textures=["0", "add", "none", "none", "none", "none", "none", "none"],
        flags=-8, indices=[0, 1, 2, 3],
        origin=(-1.5, 2.0, 3.0), u=(0.0, 65536.0, 0.0),
        v=(-65536.0, 0.0, 0.0), normal=(0.0, 0.0, 65536.0),
        spare=(0.0, 16539982.0, 0.0))
    vertices = [(0.0, 0.0, 0.0), (64.0, 0.0, 0.0),
                (64.0, 64.0, 0.0), (0.0, 64.0, 8.0)]
    return oracle.Sob(vertices, [face0, face1], "SYNTH")


def _synth_variant_b() -> bytes:
    """Variant B: sentinel ``-1`` on the integer line, no newline after plane.

    The ``-1`` that closes the integer line *is* plane number 0 (origin.x) for
    the parser and the engine, so fourteen more numbers follow it.
    """
    return (b"3 2\r\n"
            b" 0  0  0  64  0  0  64  64  0 \r\n"
            b"TICK none none none none none none none "
            b" 96  3  0  1  2  -1  0  0  65536  0  0  0  65536  0  0  0  65536 "
            b" 0  12345678  0 "
            b"next none none none none none none none "
            b" 0  3  0  1  2  -1  0  0  65536  0  0  0  65536  0  0  0  65536 "
            b" 0  12345678  0 \r\n")


def _synth_sentinel() -> bytes:
    """One face, a genuine lone ``-1`` *after* the fifteen plane numbers."""
    return (b"3 1\r\n"
            b"0 0 0 1 0 0 0 1 0\r\n"
            b"TICK none none none none none none none\r\n"
            b"0 3 0 1 2\r\n"
            b"0 0 0 65536 0 0 0 65536 0 0 0 65536 0 0 0 -1\r\n")


def _tmpdir(tag: str) -> str:
    base = os.environ.get("SOBWRITE_TMP", "/tmp/opencode")
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = tempfile.gettempdir()
    return tempfile.mkdtemp(prefix=f"sobwrite-{tag}-", dir=base)


def selftest() -> tuple[bool, list[dict]]:
    results: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append({"check": name, "ok": bool(ok), "detail": detail})

    def expect_error(name: str, fn, kind=EditError) -> None:
        try:
            fn()
        except kind as exc:
            check(name, True, str(exc))
        except Exception as exc:                       # noqa: BLE001
            check(name, False, f"wrong exception {type(exc).__name__}: {exc}")
        else:
            check(name, False, "no error raised")

    data = _synth_model().to_bytes()
    loc = walk(data, "SYNTH.SOB")

    # 1. tokenisation
    check("token: split() agrees with the byte scanner",
          [t.text for t in loc.tokens] == data.decode("latin1").split(),
          f"{len(loc.tokens)} tokens")
    manual = tokenize(b"a  b\r\nc\td")
    check("token: spans slice back",
          [b"a", b"b", b"c", b"d"]
          == [b"a"[:1], b"b"[:1], b"c"[:1], b"d"[:1]]
          and [manual[i].start for i in range(4)] == [0, 3, 6, 8],
          str([t.span for t in manual]))
    check("token: re-join is byte-exact", rejoin(data, loc.tokens) == data)
    check("token: exotic separators split like str.split()",
          [t.text for t in tokenize("a\x85b\xa0c".encode("latin1"))]
          == "a\x85b\xa0c".split())

    # 2. canonical model
    model = oracle_parse(data, "SYNTH.SOB")
    check("synth: oracle parses", (len(model.vertices),
                                   len(model.faces)) == (4, 2))
    check("synth: canonical text is the oracle layout",
          model.to_bytes() == data)
    check("synth: own walker counts", (loc.nverts, loc.nfaces) == (4, 2))
    check("synth: own vertices equal the oracle",
          loc.vertices == model.vertices)
    check("synth: own faces equal the oracle",
          all(face.textures == golden.textures and face.flags == golden.flags
              and face.indices == golden.indices
              and face.planes == [*golden.origin, *golden.u, *golden.v,
                                  *golden.normal, *golden.spare]
              for face, golden in zip(loc.faces, model.faces)))
    check("layout: counts are tokens 0 and 1",
          (loc.count_toks[0].text, loc.count_toks[1].text) == ("4", "2"))
    check("layout: first vertex token span",
          data[loc.vertex_toks[0][0].start:loc.vertex_toks[0][0].end] == b"0")
    check("layout: face 0 plane tokens are 15",
          len(loc.faces[0].plane_toks) == 15)
    check("layout: face 1 flags token is -8", loc.faces[1].flags == -8)
    structure = structure_report(loc, model)
    check("structure: own walker and oracle agree", structure["ok"])
    wrong_header = data.replace(b" 4 \r\n 2 \r\n", b" 4 \r\n 3 \r\n", 1)
    wrong = structure_report(walk(wrong_header, "BAD.SOB"),
                             oracle_parse(wrong_header, "BAD.SOB"))
    check("structure: a header face-count mismatch is reported",
          not wrong["ok"]
          and any(not c["ok"] and "header counts" in c["check"]
                  for c in wrong["checks"]))

    # 3. shipped variants
    variant = _synth_variant_b()
    vloc = walk(variant, "B.SOB")
    vmodel = oracle_parse(variant, "B.SOB")
    check("variant B: parses to EOF and re-joins",
          rejoin(variant, vloc.tokens) == variant and vloc.nfaces == 2)
    check("variant B: the integer-line -1 becomes origin.x (like the engine)",
          vloc.faces[0].origin[0] == -1.0
          and vmodel.faces[0].origin[0] == -1.0)
    check("variant B: no lone sentinel is reported",
          all(face.extra_tok is None for face in vloc.faces))
    check("variant B: faces equal the oracle",
          [list(face.origin) for face in vloc.faces]
          == [list(face.origin) for face in vmodel.faces])
    sentinel = _synth_sentinel()
    sloc = walk(sentinel, "S.SOB")
    smodel = oracle_parse(sentinel, "S.SOB")
    check("sentinel: the lone -1 after the planes is consumed",
          sloc.faces[0].extra_tok is not None
          and sloc.faces[0].extra_tok.text == "-1")
    check("sentinel: plane values match the oracle",
          sloc.faces[0].planes == [*smodel.faces[0].origin, *smodel.faces[0].u,
                                   *smodel.faces[0].v, *smodel.faces[0].normal,
                                   *smodel.faces[0].spare])
    check("sentinel: re-join is byte-exact", rejoin(sentinel, sloc.tokens) == sentinel)

    # 4. splice identity over every token
    identity = splice(data, [(tok, tok.text) for tok in loc.tokens])
    check("splice: replacing every token by itself is a no-op", identity == data)

    # 5. edits change only the target token
    cases = [
        ("vertex.0.x", "vertex.0.x=-1.5"),
        ("vertex.1", "vertex.1=65,1,2"),
        ("face.0.flags", "face.0.flags=127"),
        ("face.1.flags", "face.1.flags=-128"),
        ("face.0.texture", "face.0.texture=SPARTANX"),
        ("face.1.slot1", "face.1.slot1=translucent"),
        ("face.0.normal", "face.0.normal=0,0,65535"),
        ("face.0.spare", "face.0.spare=0,16539982,0"),
    ]
    for name, assignment in cases:
        try:
            spec = resolve_field(loc, name)
            value = parse_assignment(spec, assignment)
            validate(spec, value)
            new_bytes, _ = patch(loc, spec, value)
            report = verify_edit(loc, new_bytes, spec, value)
            check(f"edit: {assignment}", report["ok"],
                  "; ".join(c["detail"] for c in report["checks"]
                            if not c["ok"]))
        except Exception as exc:                       # noqa: BLE001
            check(f"edit: {assignment}", False, str(exc))

    # 6. the changed-byte count is honest about text width
    spec = resolve_field(loc, "face.1.flags")
    new_bytes, _ = patch(loc, spec, 8)
    rep = changed_bytes(loc, spec, new_bytes)
    check("edit: -8 -> 8 replaces one byte and drops the sign (2, -1)",
          rep["changed_bytes"] == 2 and rep["size_delta"] == -1,
          f"{rep['changed_bytes']} byte(s), {rep['size_delta']:+d}")
    spec = resolve_field(loc, "face.0.flags")
    new_bytes, _ = patch(loc, spec, 90)
    rep = changed_bytes(loc, spec, new_bytes)
    check("edit: 96 -> 90 changes exactly one byte, size unchanged",
          rep["changed_bytes"] == 1 and rep["size_delta"] == 0,
          f"{rep['changed_bytes']} byte(s), {rep['size_delta']:+d}")
    check("edit: the oracle sees the new flag",
          oracle_parse(new_bytes).faces[0].flags == 90)
    check("edit: the source bytes are untouched", walk(data, "SYNTH.SOB") == loc)

    # 7. no-op edit
    spec = resolve_field(loc, "face.0.flags")
    same, _ = patch(loc, spec, 96)
    report = verify_edit(loc, same, spec, 96)
    check("edit: writing the current value is a byte-exact no-op",
          same == data and report["ok"])

    # 8. refusals
    expect_error("refuse: vertex index out of range",
                 lambda: resolve_field(loc, "vertex.9.x"))
    expect_error("refuse: face index out of range",
                 lambda: resolve_field(loc, "face.9.origin"))
    expect_error("refuse: unknown field",
                 lambda: resolve_field(loc, "face.0.bogus"))
    expect_error("refuse: unknown axis",
                 lambda: resolve_field(loc, "vertex.0.w"))
    expect_error("refuse: vertex NaN",
                 lambda: validate(resolve_field(loc, "vertex.0.x"),
                                  parse_float("nan", "x")))
    expect_error("refuse: vertex inf",
                 lambda: validate(resolve_field(loc, "vertex.0.y"),
                                  parse_float("inf", "y")))
    expect_error("refuse: vertex implausible",
                 lambda: validate(resolve_field(loc, "vertex.0.x"), 1e10))
    expect_error("refuse: flags above 127",
                 lambda: validate(resolve_field(loc, "face.0.flags"), 128))
    expect_error("refuse: flags below -128",
                 lambda: validate(resolve_field(loc, "face.0.flags"), -129))
    expect_error("refuse: texture with a space",
                 lambda: parse_assignment(resolve_field(loc, "face.0.texture"),
                                          "face.0.texture=BIG TICK"))
    expect_error("refuse: empty texture",
                 lambda: parse_assignment(resolve_field(loc, "face.0.texture"),
                                          "face.0.texture="))
    expect_error("refuse: two-number vector",
                 lambda: parse_assignment(resolve_field(loc, "face.0.origin"),
                                          "face.0.origin=1,2"))
    expect_error("refuse: non -.sob input", lambda: require_sob("/tmp/x.txt"))
    expect_error("refuse: empty file",
                 lambda: walk(b""), ScanError)
    expect_error("refuse: huge vertex count",
                 lambda: walk(b"999999999 0 0 0 0"), ScanError)
    expect_error("refuse: truncated vertex list",
                 lambda: walk(b"4 0 0 0 0"), ScanError)
    expect_error("refuse: bad face record (nverts=2)",
                 lambda: walk(b"3 1 0 0 0 1 0 0 0 1 0 "
                              b"TICK none none none none none none none "
                              b"0 2 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"),
                 ScanError)
    expect_error("refuse: face index out of range",
                 lambda: walk(b"3 1 0 0 0 1 0 0 0 1 0 "
                              b"TICK none none none none none none none "
                              b"0 3 0 1 9 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"),
                 ScanError)
    expect_error("refuse: trailing token",
                 lambda: walk(data + b" extra"), ScanError)
    expect_error("refuse: writing into the game directory",
                 lambda: refuse_inside_game(
                     os.path.join(DEFAULT_GAME, "SOBS", "X.sob"), DEFAULT_GAME))
    check("paths: a scratch output is allowed",
          refuse_inside_game(os.path.join(_tmpdir("refuse"), "X.sob"),
                             DEFAULT_GAME) is None)

    # 9. set writes only --out, never the source
    work = _tmpdir("set")
    try:
        src = os.path.join(work, "SYNTH.sob")
        out = os.path.join(work, "SYNTH-edited.sob")
        write_bytes(src, data)
        before = read_bytes(src)
        rep = set_field(src, "face.0.flags=100", out)
        check("set: writes --out and keeps the source",
              read_bytes(src) == before and os.path.isfile(out)
              and read_bytes(out) != before, f"{rep['changed_bytes']} byte(s)")
        check("set: report is verified", rep["ok"] and rep["changed_bytes"] >= 1)
        expect_error("set: refuses --out == input",
                     lambda: set_field(src, "face.0.flags=100", src))
        check("check: identical file + --against is OK",
              check_file(src, src, None, game=DEFAULT_GAME)["ok"])
        rep = check_file(out, src, "face.0.flags=100", game=DEFAULT_GAME)
        check("check: edited file + --field is OK", rep["ok"],
              "; ".join(c["detail"] for c in rep.get("checks", [])
                        if not c["ok"]))
        rep = check_file(out, src, "face.0.flags=101", game=DEFAULT_GAME)
        check("check: a wrong expected value is caught", not rep["ok"])
        rep = check_file(out, src, None, game=DEFAULT_GAME)
        check("check: byte difference is reported without --field",
              (not rep["ok"]) and rep["diff"]["changed_bytes"] >= 1,
              str(rep.get("diff")))
        broken = os.path.join(work, "BROKEN.sob")
        write_bytes(broken, b"999999 0")
        expect_error("check: broken counts are refused",
                     lambda: check_file(broken, None, None, game=DEFAULT_GAME), ScanError)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # 10. round-trip on a synthetic corpus
    work = _tmpdir("rt")
    try:
        write_bytes(os.path.join(work, "A.SOB"), data)
        write_bytes(os.path.join(work, "B.SOB"), variant)
        write_bytes(os.path.join(work, "C.SOB"), sentinel)
        rep = roundtrip_corpus(work)
        check("roundtrip: 3/3 synthetic models are byte-identical",
              rep["identical"] == 3 and rep["files"] == 3 and rep["ok"],
              f"{rep['identical']}/{rep['files']}, {rep['bytes']} bytes")
        audit = audit_corpus(work)
        check("audit: synthetic corpus is counted",
              audit["files"] == 3 and audit["faces"] == 2 + 2 + 1
              and audit["ok"], f"{audit['faces']} faces")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    ok = all(item["ok"] for item in results)
    return ok, results


def print_selftest(ok: bool, results: list[dict]) -> None:
    for item in results:
        mark = "ok  " if item["ok"] else "FAIL"
        print(f"  [{mark}] {item['check']}"
              + (f" ({item['detail']})" if item["detail"] else ""))
    passed = sum(1 for item in results if item["ok"])
    print(f"self-test: {passed}/{len(results)} passed")
    print(f"result: {'OK' if ok else 'FAIL'}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_methods(ap) -> None:
    ap.add_argument("file", nargs="?", help=".sob file (input)")
    ap.add_argument("--list", action="store_true", help="list one model")
    ap.add_argument("--get", action="store_true", help="read one field")
    ap.add_argument("--set", action="store_true",
                    help="edit one field into --out")
    ap.add_argument("--check", action="store_true",
                    help="re-read and verify (optionally against a source)")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every .sob under DIR/game")
    ap.add_argument("--audit", action="store_true",
                    help="corpus statistics")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests, no game data")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: source file to compare with")
    ap.add_argument("--field", metavar="NAME[=VALUE]",
                    help="field name; --set/--check need NAME=VALUE")
    ap.add_argument("--out", metavar="PATH", help="--set: output .sob")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL", DEFAULT_GAME),
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=16,
                    help="--list: faces shown (0 = all)")
    ap.add_argument("--json", action="store_true", help="JSON report")


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv:
        head = argv[0].lower()
        aliases = {
            "self-test": "--self-test", "selftest": "--self-test",
            "roundtrip": "--roundtrip", "audit": "--audit",
            "list": "--list", "get": "--get", "set": "--set", "check": "--check",
        }
        if head in aliases:
            argv = [aliases[head]] + argv[1:]
    ap = argparse.ArgumentParser(
        description="Layout-preserving .sob writer/editor (M11.5, slice 2).")
    _add_methods(ap)
    args = ap.parse_args(argv)

    modes = [args.list, args.get, args.set, args.check, args.roundtrip,
             args.audit, args.self_test]
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

        if args.roundtrip:
            root = args.file or args.game
            rep = roundtrip_corpus(root, verbose=False)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1

        if args.audit:
            rep = audit_corpus(args.file or args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_audit(rep)
            return 0 if rep["ok"] else 1

        if not args.file:
            raise EditError("this mode needs a .sob file")

        if args.list:
            rep = list_file(args.file, max(args.limit, 0))
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_list(rep, max(args.limit, 0))
            return 0 if rep["structure"]["ok"] else 1

        if args.get:
            if not args.field:
                raise EditError("--get needs --field NAME")
            rep = get_field(args.file, args.field)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_get(rep)
            return 0

        if args.set:
            if not args.field:
                raise EditError("--set needs --field NAME=VALUE")
            if not args.out:
                raise EditError("--set needs --out FILE")
            rep = set_field(args.file, args.field, args.out, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_set(rep)
            return 0 if rep["ok"] else 1

        if args.check:
            rep = check_file(args.file, args.against, args.field, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_check(rep)
            return 0 if rep["ok"] else 1

        ap.print_help()
        return 2
    except EditError as exc:
        print(f"sobwrite: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"sobwrite: corrupt .sob: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"sobwrite: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                           # noqa: BLE001
        print(f"sobwrite: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
