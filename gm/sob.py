"""Gunmetal ".sob" object models (and the ".soe" entity tables).

.sob is plain DOS text, but **line breaks carry no meaning** — the file is a
flat whitespace separated token stream.  Any line-oriented parser mis-parses
the moment a texture name lands at the end of a "plane" line, which happens in
roughly half of the shipped files.

Grammar (verified: all 672 files under SOBS/ parse and the stream is consumed
exactly):

    <vertex_count> <face_count>
    vertex_count * (x y z)                       floats, world units
    face_count * {
        8 name tokens                           [0] = diffuse texture,
                                                [1..7] = material flags
                                                ("none", "translucent", "add")
        flags nverts i0 .. i(nverts-1)          integer line
        15 numbers                              origin(3) U(3) V(3) N(3) spare(3)
        [-1]                                    optional lone sentinel
    }

The optional lone `-1` *after* the fifteen plane numbers cannot be told apart
locally, so the parser backtracks; the search is memoised over the token
position, so it stays linear.  The `-1` that most shipped files put at the end
of the integer line is read as the first plane number (origin.x == -1), exactly
like the engine's loader does.

The U/V/N vectors are unit axes scaled by 65536 (the engine's 16.16 planar
texture projection), i.e. UV = dot(p - origin, axis) / 65536 as a first cut.

Writer
------
``Sob.to_bytes()`` (alias ``encode()``) emits the canonical layout::

    " 62 \r\n 43 \r\n"                        counts, one per line
    " 12  28  18 \r\n"                        3 vertices per line
    "TICK\r\nnone\r\n..."                     8 texture tokens, one per line
    " 96  6  10  8  6  4  2  0 \r\n"          flags, nverts, indices
    " 12928  6400 ... 12345678  0 \r\n"       15 plane numbers

This is the clean variant A of the shipped files (77 of 672 match it byte for
byte); most shipped files put the first texture token at the end of a plane
line, and a few use another float spelling — see docs/SOB.md §2.2.

Every number is written as ``"% d "`` would do in C — a space takes the sign's
place for non-negative values, floats are shortest-round-trip (``repr``) and
integral values are printed without a decimal point, so parsing the output
yields the same model.  ``Sob.raw`` keeps the bytes the model came from, so
``write()`` reproduces an unmodified file byte for byte; ``rebuild()`` forces
the canonical text (use it after editing).

.soe files are entity ("thing") templates: a token stream with a different,
still partially unknown shape — parse_soe() returns the raw tokens.
"""

from __future__ import annotations

import math
import os
import sys
import tempfile
from dataclasses import dataclass, field

TEX_SLOTS = 8
PLANE_NUMBERS = 15
PLANE_SCALE = 65536.0


@dataclass
class SobFace:
    textures: list[str]
    flags: int
    indices: list[int]
    origin: tuple[float, float, float]
    u: tuple[float, float, float]
    v: tuple[float, float, float]
    normal: tuple[float, float, float]
    spare: tuple[float, float, float]

    @property
    def texture(self) -> str:
        return self.textures[0]

    @property
    def material_flags(self) -> list[str]:
        return [t for t in self.textures[1:] if t != "none"]


@dataclass
class Sob:
    vertices: list[tuple[float, float, float]]
    faces: list[SobFace] = field(default_factory=list)
    name: str = ""
    #: Bytes the model was parsed from (``None`` for built/edited models).
    #: ``write()`` returns them unchanged while the model still matches, which
    #: makes an untouched round-trip byte-exact even where the shipped file's
    #: layout differs from the canonical one (see docs/SOB.md).
    raw: bytes | None = field(default=None, repr=False, compare=False)

    def uv(self, face: SobFace, vertex) -> tuple[float, float]:
        d = (
            vertex[0] - face.origin[0],
            vertex[1] - face.origin[1],
            vertex[2] - face.origin[2],
        )
        su = sum(a * b for a, b in zip(d, face.u)) / PLANE_SCALE
        sv = sum(a * b for a, b in zip(d, face.v)) / PLANE_SCALE
        return su, sv

    def to_obj(self, mtl: str | None = None, triangulate: bool = False) -> str:
        out = ["# Gunmetal .sob export", f"o {self.name or 'object'}"]
        if mtl:
            out.append(f"mtllib {mtl}")
        for x, y, z in self.vertices:
            out.append(f"v {x:g} {z:g} {-y:g}")  # Gunmetal is Z-up, OBJ is Y-up

        for fi, f in enumerate(self.faces):
            out.append(f"usemtl {f.texture}")
            out.append(f"vn {f.normal[0]:g} {f.normal[2]:g} {-f.normal[1]:g}")
            base = len(self.vertices) + 3 * fi + 1
            idx = []
            for k, vi in enumerate(f.indices):
                u, v = self.uv(f, self.vertices[vi])
                out.append(f"vt {u:.5f} {v:.5f}")
                idx.append((vi + 1, base + k))
            if triangulate and len(idx) > 3:
                for k in range(1, len(idx) - 1):
                    tri = (idx[0], idx[k], idx[k + 1])
                    out.append("f " + " ".join(f"{a}/{b}/1" for a, b in tri))
            else:
                out.append("f " + " ".join(f"{a}/{b}/1" for a, b in idx))
        return "\n".join(out) + "\n"

    def to_mtl(self) -> str:
        seen: list[str] = []
        for f in self.faces:
            if f.texture not in seen:
                seen.append(f.texture)
        lines = ["# Gunmetal .sob materials"]
        for t in seen:
            lines += [f"newmtl {t}", "Kd 1.000 1.000 1.000", "d 1.0", "illum 1", ""]
        return "\n".join(lines)

    # -- writing -----------------------------------------------------------

    def content_equal(self, other: "Sob") -> bool:
        """True when both models carry the same vertices, faces and textures."""
        if self.vertices != other.vertices or len(self.faces) != len(other.faces):
            return False
        for a, b in zip(self.faces, other.faces):
            if (a.textures != b.textures or a.flags != b.flags
                    or a.indices != b.indices):
                return False
            if (a.origin, a.u, a.v, a.normal, a.spare) != (
                    b.origin, b.u, b.v, b.normal, b.spare):
                return False
        return True

    def is_unmodified(self) -> bool:
        """True when the model still matches the bytes it was parsed from."""
        if self.raw is None:
            return False
        try:
            return self.content_equal(parse_data(self.raw))
        except ValueError:
            return False

    def to_bytes(self, newline: str = "\r\n") -> bytes:
        """Serialise to the canonical `.sob` text (parseable back losslessly).

        The layout follows the shipped files: counts on their own lines,
        three vertices per line, eight texture tokens one per line, then the
        integer line and fifteen plane numbers per face.  Numbers use the
        game's ``"% d "`` spacing (a space stands where a minus would be) and
        floats use the shortest round-trip form, so no information is lost.
        """
        if len(self.vertices) > MAX_VERTS:
            raise ValueError(f"{self.name}: {len(self.vertices)} vertices "
                             f"exceed the limit {MAX_VERTS}")
        if len(self.faces) > MAX_FACES:
            raise ValueError(f"{self.name}: {len(self.faces)} faces "
                             f"exceed the limit {MAX_FACES}")
        out: list[str] = [_fmt_field(len(self.vertices)) + newline,
                          _fmt_field(len(self.faces)) + newline]
        for n, (x, y, z) in enumerate(self.vertices):
            try:
                out.append(_fmt_field(x) + _fmt_field(y) + _fmt_field(z) + newline)
            except ValueError as exc:
                raise ValueError(f"{self.name}: bad vertex {n}: {exc}") from exc
        for n, f in enumerate(self.faces):
            if len(f.textures) != TEX_SLOTS:
                raise ValueError(f"{self.name}: face {n} has {len(f.textures)} "
                                 f"texture slots, expected {TEX_SLOTS}")
            for t in f.textures:
                if not t or any(c.isspace() for c in t):
                    raise ValueError(f"{self.name}: face {n} has an unusable "
                                     f"texture token {t!r}")
            out.extend(t + newline for t in f.textures)
            if not 3 <= len(f.indices) <= 32:
                raise ValueError(f"{self.name}: face {n} has {len(f.indices)} "
                                 "indices, the format allows 3..32")
            for i in f.indices:
                if not 0 <= i < len(self.vertices):
                    raise ValueError(f"{self.name}: face {n} references vertex "
                                     f"{i}, the model has {len(self.vertices)}")
            ints = [f.flags, len(f.indices), *f.indices]
            out.append("".join(_fmt_field(v) for v in ints) + newline)
            plane = [*f.origin, *f.u, *f.v, *f.normal, *f.spare]
            out.append("".join(_fmt_field(v) for v in plane) + newline)
        return "".join(out).encode("latin1")

    def encode(self) -> bytes:
        """Alias of :meth:`to_bytes` for built or edited models."""
        return self.to_bytes()

    def write(self, path: str, force: bool = False, exact: bool = True) -> bytes:
        """Write the model atomically and return the bytes that were stored.

        ``exact`` keeps the original bytes when the model is unmodified
        (byte-exact round-trip).  Existing files are only replaced with
        ``force``; otherwise ``FileExistsError`` is raised.
        """
        data = self.to_bytes()
        if exact and self.raw is not None and self.is_unmodified():
            data = self.raw
        if not force and os.path.exists(path):
            raise FileExistsError(f"{path}: exists (use --force to overwrite)")
        atomic_write(path, data)
        return data

    def rebuild(self, path: str, force: bool = False) -> bytes:
        """Rewrite the model in canonical form (drops the original layout)."""
        return self.write(path, force=force, exact=False)

    def move_vertex(self, index: int, dx: float = 0.0, dy: float = 0.0,
                    dz: float = 0.0) -> tuple[float, float, float]:
        """Offset one vertex in place; returns its new position."""
        if not 0 <= index < len(self.vertices):
            raise IndexError(f"{self.name}: no vertex {index} "
                             f"(model has {len(self.vertices)})")
        x, y, z = self.vertices[index]
        moved = (x + dx, y + dy, z + dz)
        self.vertices[index] = moved
        return moved


def _fmt_number(value: int | float) -> str:
    """One `.sob` number: integers bare, other values shortest round-trip."""
    if isinstance(value, int):
        return str(value)
    f = float(value)
    if not math.isfinite(f):
        raise ValueError(f"non-finite number {f!r}")
    if f == 0.0:
        return "0"
    if f.is_integer() and abs(f) < 1e16:
        return str(int(f))
    return repr(f)


def _fmt_field(value: int | float) -> str:
    """One number with the game's ``"% d "`` padding (space where ``-`` goes)."""
    text = _fmt_number(value)
    return (" " + text if value >= 0 else text) + " "


def atomic_write(path: str, data: bytes) -> None:
    """Replace ``path`` with ``data`` via a temporary file in the same folder."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".sob-", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _is_int(tok: str) -> bool:
    try:
        int(tok)
        return True
    except ValueError:
        return False


def _is_float(tok: str) -> bool:
    try:
        float(tok)
        return True
    except ValueError:
        return False


#: Trust-boundary guards.  The largest shipped model has 957 vertices / 1130
#: faces; the parser recurses once per face, so hostile counts must be rejected
#: before they can exhaust the C stack or drive a huge vertex loop.
MAX_VERTS = 200_000
MAX_FACES = 5_000


def parse_tokens(tokens: list[str], name: str = "") -> Sob:
    if len(tokens) < 2:
        raise ValueError(f"{name}: .sob has fewer than two numbers")
    try:
        nverts = int(tokens[0])
        nfaces = int(tokens[1])
    except ValueError as exc:
        raise ValueError(f"{name}: bad vertex/face count") from exc
    if not 0 <= nverts <= MAX_VERTS or not 0 <= nfaces <= MAX_FACES:
        raise ValueError(f"{name}: implausible counts {nverts} vertices / {nfaces} faces")
    if 2 + 3 * nverts > len(tokens):
        raise ValueError(f"{name}: truncated vertex list")
    # Each face needs at least 8 texture tokens + flags/nv + 3 indices + 15
    # plane numbers; reject counts the stream cannot possibly hold.
    if 2 + 3 * nverts + nfaces * 28 > len(tokens) + 27:
        raise ValueError(f"{name}: face count {nfaces} does not fit the stream")
    pos = 2
    # walk() recurses once per face; the largest model (MOCLASS.SOB) has 1130.
    if sys.getrecursionlimit() < nfaces * 4 + 200:
        sys.setrecursionlimit(nfaces * 4 + 200)

    vertices = []
    for _ in range(nverts):
        x, y, z = (float(t) for t in tokens[pos : pos + 3])
        vertices.append((x, y, z))
        pos += 3

    memo: dict[int, int | None] = {}
    extra_of: dict[int, int] = {}

    def walk(p: int) -> int | None:
        """Return the token position after a valid tail starting at p, or None."""
        if p in memo:
            return memo[p]
        memo[p] = None  # guard against re-entry
        if p == len(tokens):
            memo[p] = p
            return p
        try:
            head = tokens[p : p + TEX_SLOTS]
            if len(head) < TEX_SLOTS:
                return None
            q = p + TEX_SLOTS
            flags = int(tokens[q])
            nv = int(tokens[q + 1])
            if nv < 3 or nv > 32:
                return None
            for k in range(2, 2 + nv):
                i = int(tokens[q + k])
                if not 0 <= i < nverts:
                    return None
            q += 2 + nv
            for _ in range(PLANE_NUMBERS):
                if not _is_float(tokens[q]):
                    return None
                q += 1
        except (ValueError, IndexError):
            return None
        for extra in (0, 1):
            r = q + extra
            if extra and (r > len(tokens) or tokens[q] != "-1"):
                continue
            res = walk(r)
            if res is not None:
                memo[p] = res
                extra_of[p] = extra
                return res
        return None

    end = walk(pos)
    if end is None:
        raise ValueError(f"{name}: no valid parse from token {pos}")

    # Re-walk now that we know a solution exists, collecting the faces.
    p = pos
    faces: list[SobFace] = []
    while p < end:
        face_textures = tokens[p : p + TEX_SLOTS]
        q = p + TEX_SLOTS
        flags = int(tokens[q])
        nv = int(tokens[q + 1])
        indices = [int(t) for t in tokens[q + 2 : q + 2 + nv]]
        q += 2 + nv
        plane = [float(t) for t in tokens[q : q + PLANE_NUMBERS]]
        q += PLANE_NUMBERS
        # walk() records a lone "-1" *after* the fifteen plane numbers, so the
        # skip must happen here too (no shipped file uses it, but the writer's
        # round-trip guarantee must hold for every stream the parser accepts).
        if extra_of.get(p):
            q += 1
        p = q
        faces.append(
            SobFace(
                textures=face_textures,
                flags=flags,
                indices=indices,
                origin=tuple(plane[0:3]),
                u=tuple(plane[3:6]),
                v=tuple(plane[6:9]),
                normal=tuple(plane[9:12]),
                spare=tuple(plane[12:15]),
            )
        )

    if p != end:
        raise ValueError(f"{name}: parser stopped at {p}, expected {end}")
    if end != len(tokens):
        raise ValueError(f"{name}: {len(tokens) - end} trailing tokens")

    return Sob(vertices, faces, name or os.path.splitext(os.path.basename(name))[0])


def parse_data(data: bytes, name: str = "") -> Sob:
    """Parse `.sob` bytes, keeping ``data`` as :attr:`Sob.raw`."""
    tokens = data.decode("latin1").split()
    sob = parse_tokens(tokens, name=name)
    sob.raw = data
    return sob


def parse(path: str) -> Sob:
    with open(path, "rb") as fh:
        data = fh.read()
    return parse_data(data, name=os.path.splitext(os.path.basename(path))[0])


def to_obj_file(path: str, out_dir: str) -> str:
    sob = parse(path)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, sob.name + ".obj"), "w", encoding="utf-8") as f:
        f.write(sob.to_obj(mtl=sob.name + ".mtl"))
    with open(os.path.join(out_dir, sob.name + ".mtl"), "w", encoding="utf-8") as f:
        f.write(sob.to_mtl())
    return sob.name


def parse_soe(path: str) -> dict:
    with open(path, "r", encoding="latin1", newline="") as f:
        tokens = f.read().split()
    return {"path": path, "tokens": tokens}
