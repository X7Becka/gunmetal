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
        [-1]                                    optional sentinel, see below
        15 numbers                              origin(3) U(3) V(3) N(3) spare(3)
    }

The integer line is followed by an optional lone `-1` in newer files.  Whether
it is present cannot be decided locally, so the parser backtracks; the search
is memoised over the token position, so it stays linear.

The U/V/N vectors are unit axes scaled by 65536 (the engine's 16.16 planar
texture projection), i.e. UV = dot(p - origin, axis) / 65536 as a first cut.

.soe files are entity ("thing") templates: a token stream with a different,
still partially unknown shape — parse_soe() returns the raw tokens.
"""

from __future__ import annotations

import os
import sys
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
        if extra_of.get(p):
            q += 1
        plane = [float(t) for t in tokens[q : q + PLANE_NUMBERS]]
        p = q + PLANE_NUMBERS
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


def parse(path: str) -> Sob:
    with open(path, "r", encoding="latin1", newline="") as f:
        tokens = f.read().split()
    return parse_tokens(tokens, name=os.path.splitext(os.path.basename(path))[0])


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
