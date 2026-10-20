"""Minimal software renderer for `.sob` meshes (and their OBJ twins).

No third-party dependencies: a perspective camera, a z-buffer, scanline
rasterisation of triangulated faces and nearest-neighbour texturing from the
game's own archives (``PICS/SBM.IDX``, then ``sprites/SBM.IDX``).

Each `.sob` face carries a plane basis (``origin``/``u``/``v``, 16.16), so UVs
come straight from :meth:`gm.sob.Sob.uv`; by default the face's own UV extent
is mapped onto the whole texture (``uv="fit"``) which looks best on small
patches, while ``uv="tile"`` repeats the texture per world unit.

Typical use::

    from gm import render, sob
    pool = render.TexturePool("/mnt/d/games/Gunmetal 3dfx")
    png_bytes = render.render(sob.parse(open("tank.sob").read()), 480,
                              pool=pool, az=35, el=22)
"""
from __future__ import annotations

import math
import os

from . import bigfile, palette as palmod, png, sbm, sob as sobmod

# ---------------------------------------------------------------------------
# mesh adapters: `.sob` models and `.sbw` level geometry look the same to the
# renderer (vertices + faces + a per-face UV function)
# ---------------------------------------------------------------------------


class LevelFace:
    """A `.sbw` polygon seen as a render face."""

    __slots__ = ("indices", "texture", "normal", "u", "v", "origin",
                 "material", "brightness", "overlay", "lightmap", "uv_fp")

    def __init__(self, poly, name: str):
        self.indices = list(poly.indices)
        self.texture = name
        self.normal = poly.normal
        self.u = poly.u_axis
        self.v = poly.v_axis
        self.origin = poly.centre
        self.material = poly.material
        self.brightness = poly.brightness
        self.overlay = poly.overlay
        # lightmap record (0-based) and the fixed-point UV offset (poly+0x54/+0x58)
        self.lightmap = poly.lightmap - 1 if poly.lightmap > 0 else -1
        self.uv_fp = (_s32(poly.uv_u_fp), _s32(poly.uv_v_fp))


class Lightmaps:
    """The ``.sbc`` records of one level, sampled the way the engine does.

    Polygon ``lightmap`` (``+0x04``) is a 1-based index into the record list;
    the engine fixes it up by the running base (``0x428064``) and then samples
    with (``0x43ca5a``..``0x43cc9c``)::

        u = (dot(u_axis, vertex - centre) * 64 + (int)uv_u_fp) / 2048
        v = (dot(v_axis, vertex - centre) * 64 + (int)uv_v_fp) / 2048
        light = pixels[(v & (h-1)) * 256 + (u & (w-1))]   # row pitch is 256

    confirmed on 99.6% of 10 070 vertex samples of ``level0``/``dm1`` (the rest
    overhang the page by a texel and wrap through the mask, as in the engine).
    """

    __slots__ = ("records",)

    def __init__(self, records=()):
        self.records = list(records)

    def uv(self, face, vertex, mask: bool = True) -> tuple[int, int] | None:
        rec = self.records[face.lightmap] if 0 <= face.lightmap < len(self.records) else None
        if rec is None:
            return None
        d = _sub(vertex, face.origin)
        # engine: u = (dot(u_axis, v-centre) * 64 + uv_u_fp) / 2 ; texel = ftol(u) >> 10
        u = int((_dot(d, face.u) * 64.0 + face.uv_fp[0]) / 2.0) >> 10
        v = int((_dot(d, face.v) * 64.0 + face.uv_fp[1]) / 2.0) >> 10
        if mask:                      # the engine wraps with the page mask
            u &= rec.width - 1
            v &= rec.height - 1
        return u, v

    def page(self, face):
        if 0 <= face.lightmap < len(self.records):
            return self.records[face.lightmap]
        return None


class LevelMesh:
    """One `.sbw` object (its local vertex pool + polygons)."""

    sector: int | None = None

    def __init__(self, obj, tex_names, lightmaps: Lightmaps | None = None,
                 sector: int | None = None):
        self.vertices = list(obj.vertices)
        self.lightmaps = lightmaps
        self.sector = sector
        self.faces = []
        for p in obj.polys:
            if len(p.indices) < 3:
                continue
            name = "none"
            if 0 <= p.texture < len(tex_names):
                name = tex_names[p.texture]
            self.faces.append(LevelFace(p, name))

    def uv(self, face, vertex):
        d = _sub(vertex, face.origin)
        return _dot(d, face.u), _dot(d, face.v)

    def lightmap_uv(self, face, vertex):
        return self.lightmaps.uv(face, vertex) if self.lightmaps is not None else None


def level_meshes(level, lightmaps: Lightmaps | None = None,
                 transform: bool = False) -> list[LevelMesh]:
    """Wrap every object of a parsed `.sbw` level as a renderable mesh.

    With *transform* the objects are placed in the engine world frame
    (``origin + basis * local``, see :meth:`gm.sbw.Object.world_vertices`)
    instead of the raw file (local) frame.
    """
    names = [t.name for t in level.tex]
    out = []
    for si, s in enumerate(level.sectors):
        for o in s.objects:
            if not o.vertices:
                continue
            if transform:
                out.append(PlacedMesh(o, names, lightmaps, si))
            else:
                out.append(LevelMesh(o, names, lightmaps, si))
    return out


class PlacedMesh(LevelMesh):
    """A level object placed in the engine world frame.

    ``world = origin + basis * local`` (header ``points``); the polygons (and
    their ``u``/``v`` axes) stay in the local frame, so ``uv`` first maps the
    world vertex back.  A uniform basis scale ``s`` cancels out exactly, which
    keeps the texture mapping identical to the file-frame render.
    """

    def __init__(self, obj, tex_names, lightmaps: Lightmaps | None,
                 sector: int | None):
        super().__init__(obj, tex_names, lightmaps, sector)
        self.vertices = list(obj.world_vertices())
        if len(obj.points) >= 4:
            self._origin = obj.points[0]
            b = obj.points[1]
            self._s = math.sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]) or 1.0
        else:
            self._origin = (0.0, 0.0, 0.0)
            self._s = 1.0
        self._inv = 1.0 / self._s

    def _local(self, vertex):
        return tuple((vertex[i] - self._origin[i]) * self._inv for i in range(3))

    def uv(self, face, vertex):
        d = _sub(self._local(vertex), face.origin)
        return _dot(d, face.u), _dot(d, face.v)

    def lightmap_uv(self, face, vertex):
        if self.lightmaps is None:
            return None
        return self.lightmaps.uv(face, self._local(vertex))


def import_lightmaps(path: str) -> Lightmaps:
    """Load the ``<level>.sbc`` sibling of a ``.sbw`` level."""
    from . import sbc as sbcmod

    with open(path, "rb") as fh:
        return Lightmaps(sbcmod.parse(fh.read()).records)


def model_meshes(model) -> list:
    """Wrap a parsed `.sob` as a list of renderable meshes."""
    return [model]


def place_model(model, pos, scale: float = 1.0):
    """A translated (and optionally rescaled) copy of a parsed `.sob`.

    The level's file frame is used for *pos*.  Scaling happens about the model's
    own origin: the vertices and the per-face planes are multiplied by *scale*,
    while the ``u``/``v`` axes are divided by it, so ``uv()`` (which measures
    from ``face.origin``) stays unchanged.
    """
    dx, dy, dz = pos
    s = scale
    verts = [(v[0] * s + dx, v[1] * s + dy, v[2] * s + dz)
             for v in model.vertices]
    inv = 1.0 / s if s else 1.0
    faces = [sobmod.SobFace(f.textures, f.flags, f.indices,
                            (f.origin[0] * s + dx, f.origin[1] * s + dy,
                             f.origin[2] * s + dz),
                            tuple(c * inv for c in f.u),
                            tuple(c * inv for c in f.v),
                            f.normal, f.spare)
             for f in model.faces]
    return sobmod.Sob(verts, faces, model.name)


# ---------------------------------------------------------------------------
# textures
# ---------------------------------------------------------------------------


class Texture:
    """A decoded ``.sbm`` together with the installation palette.

    ``rgb(level)`` reproduces the old level-0 conversion byte for byte, while
    :meth:`indices` keeps the raw 8-bit texels so the transparent palette index
    (the game uses 0, see ``docs/MIPMAPS.md``) can be tested during rendering.
    """

    __slots__ = ("name", "width", "height", "levels", "kind", "sbm", "palette",
                 "_rgb")

    def __init__(self, name: str, tex: sbm.Sbm, palette: bytes, kind: str = "pics"):
        self.name = name
        self.width = tex.width
        self.height = tex.height
        self.levels = len(tex.levels_data)
        self.kind = kind
        self.sbm = tex
        self.palette = palette
        self._rgb: list[tuple[int, int, bytes] | None] = [None] * self.levels

    @property
    def transparent_index(self) -> int | None:
        """Palette index the game keys out, if level 0 actually contains it."""
        return 0 if b"\x00" in self.indices(0) else None

    def size(self, level: int = 0) -> tuple[int, int]:
        level = max(0, min(int(level), self.levels - 1))
        return max(1, self.width >> level), max(1, self.height >> level)

    def indices(self, level: int = 0) -> bytes:
        """8-bit texels of *level*; a missing level falls back to level 0."""
        if 0 <= level < self.levels:
            return self.sbm.levels_data[level]
        return self.sbm.levels_data[0]

    def rgb(self, level: int = 0) -> tuple[int, int, bytes]:
        """``(width, height, rgb bytes)`` for one mip level (cached)."""
        level = max(0, min(int(level), self.levels - 1))
        cached = self._rgb[level]
        if cached is not None:
            return cached
        w, h = self.size(level)
        pix = self.indices(level)
        pal = self.palette
        n = min(256, len(pal) // 4)
        rows = []
        for i in range(min(w * h, len(pix))):
            c = pix[i] if pix[i] < n else 0
            rows.append(pal[4 * c : 4 * c + 3])
        if len(rows) < w * h:                    # truncated blob: pad
            rows += [b"\x00\x00\x00"] * (w * h - len(rows))
        cached = (w, h, b"".join(rows))
        self._rgb[level] = cached
        return cached


class TexturePool:
    """Resolve texture names to ``(width, height, rgb bytes)``.

    The archives are read lazily and cached; ``None`` means "not found" and the
    renderer then falls back to a flat colour.  :meth:`get_texture` exposes the
    full mip chain and the palette indices without changing :meth:`get`.
    """

    def __init__(self, game: str | None = None, pal: bytes | None = None,
                 archives: tuple[str, ...] = ("PICS", "sprites")):
        self.game = game
        self._palette = pal
        self._archives = archives
        self._maps: list[tuple[str, dict[str, int], list[int], str]] | None = None
        self._cache: dict[str, tuple[int, int, bytes] | None] = {}
        self._tex_cache: dict[str, Texture | None] = {}

    def _pal_bytes(self) -> bytes:
        if self._palette is None:
            self._palette = palmod.installed(self.game)
        return self._palette

    def _index(self) -> list[tuple[str, dict[str, int], list[int], str]]:
        """``[(dat_path, {name: offset}, sorted offsets, kind)]``."""
        if self._maps is None:
            self._maps = []
            for sub in self._archives:
                idx = os.path.join(self.game or "", sub, "SBM.IDX")
                if not os.path.exists(idx):
                    continue
                bf = bigfile.BigFile(idx)
                offs = {e.name: e.offset for e in bf.entries}
                kind = "sprites" if os.path.basename(sub).lower() == "sprites" else "pics"
                self._maps.append((bf.dat_path, offs, sorted(offs.values()), kind))
        return self._maps

    def get_texture(self, name: str) -> Texture | None:
        """The decoded texture with all ``.sbm`` mip levels, or ``None``."""
        if not name or name == "none":
            return None
        if name in self._tex_cache:
            return self._tex_cache[name]
        tex = None
        for dat_path, offs, order, kind in self._index():
            off = offs.get(name)
            if off is None:                      # case-insensitive retry
                up = name.upper()
                for k, v in offs.items():
                    if k.upper() == up:
                        off = v
                        break
            if off is None:
                continue
            size = _entry_size(dat_path, order, off)
            with open(dat_path, "rb") as f:
                f.seek(off)
                blob = f.read(size)
            try:
                tex = Texture(name, sbm.parse(blob), self._pal_bytes(), kind)
            except ValueError:
                tex = None
            break
        self._tex_cache[name] = tex
        return tex

    def get(self, name: str) -> tuple[int, int, bytes] | None:
        if not name or name == "none":
            return None
        if name in self._cache:
            return self._cache[name]
        tex = self.get_texture(name)
        img = None if tex is None else tex.rgb(0)
        self._cache[name] = img
        return img

    def _to_rgb(self, tex: sbm.Sbm) -> tuple[int, int, bytes]:
        return Texture("", tex, self._pal_bytes()).rgb(0)


def _entry_size(dat_path: str, order: list[int], off: int) -> int:
    """The BIGFILE index has no sizes; the next entry's offset bounds an entry."""
    lo, hi = 0, len(order)
    while lo < hi:                                # first offset > off
        mid = (lo + hi) // 2
        if order[mid] > off:
            hi = mid
        else:
            lo = mid + 1
    if lo < len(order):
        return order[lo] - off
    return os.path.getsize(dat_path) - off


# ---------------------------------------------------------------------------
# camera / raster
# ---------------------------------------------------------------------------

Vec3 = tuple[float, float, float]


def _sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _s32(v: int) -> int:
    """u32 field read as the signed int the engine's ``fild`` sees."""
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v >= 0x80000000 else v


def _norm(a: Vec3) -> Vec3:
    n = math.sqrt(_dot(a, a)) or 1.0
    return (a[0] / n, a[1] / n, a[2] / n)


def _bounds(meshes) -> tuple[Vec3, float]:
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for m in meshes:
        for v in m.vertices:
            for k in range(3):
                lo[k] = min(lo[k], v[k])
                hi[k] = max(hi[k], v[k])
    if lo[0] > hi[0]:
        return (0.0, 0.0, 0.0), 1.0
    c = tuple((lo[k] + hi[k]) / 2 for k in range(3))
    r = max(1e-6, 0.5 * math.sqrt(sum((hi[k] - lo[k]) ** 2 for k in range(3))))
    return c, r


class Camera:
    def __init__(self, center: Vec3, radius: float, width: int, height: int,
                 az: float = 35.0, el: float = 22.0, zoom: float = 1.0,
                 fov: float = 45.0, pos: Vec3 | None = None,
                 look: Vec3 | None = None):
        self.width, self.height = width, height
        if pos is not None:
            self.eye = pos
            target = look if look is not None else center
            self.fwd = _norm(_sub(target, self.eye))
            dist = max(1e-3, math.sqrt(_dot(_sub(target, self.eye),
                                            _sub(target, self.eye))))
        else:
            a, e = math.radians(az), math.radians(el)
            d = (math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e))
            self.dir = _norm(d)                   # from the object towards the eye
            dist = radius / math.sin(math.radians(fov) / 2) / max(1e-3, zoom)
            self.eye = tuple(center[k] + self.dir[k] * dist for k in range(3))
            self.fwd = _norm(_sub(center, self.eye))
        up = (0.0, 0.0, 1.0)
        if abs(_dot(self.fwd, up)) > 0.99:
            up = (0.0, 1.0, 0.0)
        self.right = _norm(_cross(self.fwd, up))
        self.up = _cross(self.right, self.fwd)
        self.scale = (height / 2) / math.tan(math.radians(fov) / 2)
        self.cx, self.cy = width / 2.0, height / 2.0
        self.near = max(0.02, min(dist, radius) * 0.05)

    def view(self, p: Vec3) -> Vec3:
        """World point -> view space (x right, y up, z forward)."""
        d = _sub(p, self.eye)
        return (_dot(d, self.right), _dot(d, self.up), _dot(d, self.fwd))

    def project_view(self, v: Vec3):
        """View point -> ``(sx, sy, inv_z, z)`` or ``None`` at/behind the eye.

        Vertices produced by :func:`_clip_near` sit exactly on the near plane,
        so only ``z <= 0`` is rejected here.
        """
        x, y, z = v
        if z <= 0.0:
            return None
        return (self.cx + x / z * self.scale, self.cy - y / z * self.scale,
                1.0 / z, z)

    def project(self, p: Vec3):
        """World point -> ``(sx, sy, inv_z, z)`` or ``None`` when behind the camera."""
        return self.project_view(self.view(p))

    def plane_limits(self) -> tuple[float, float]:
        """Half-extent of the view frustum per unit of depth (x, y)."""
        return (self.width / 2.0) / self.scale, (self.height / 2.0) / self.scale

    def aabb_visible(self, lo: Vec3, hi: Vec3) -> bool:
        """Conservative test: can a view-space AABB touch the frustum?

        The engine does the same thing per object one level down: it skips
        objects whose flags (``obj+0x70 & 0x10010``) are set and uses the
        bounding radius (``obj+0x60``) for the test (0x437d86..0x437db0).
        """
        kx, ky = self.plane_limits()
        if hi[2] < self.near:
            return False
        return not (lo[0] - kx * hi[2] > 0 or hi[0] + kx * hi[2] < 0
                    or lo[1] - ky * hi[2] > 0 or hi[1] + ky * hi[2] < 0)

    def world_aabb_visible(self, lo: Vec3, hi: Vec3) -> bool:
        """View-space AABB of a world AABB, then :meth:`aabb_visible`."""
        xs = (lo[0], hi[0])
        ys = (lo[1], hi[1])
        zs = (lo[2], hi[2])
        vlo = [1e30] * 3
        vhi = [-1e30] * 3
        for x in xs:
            for y in ys:
                for z in zs:
                    v = self.view((x, y, z))
                    for k in range(3):
                        vlo[k] = min(vlo[k], v[k])
                        vhi[k] = max(vhi[k], v[k])
        return self.aabb_visible(tuple(vlo), tuple(vhi))


# ---------------------------------------------------------------------------
# renderer
# ---------------------------------------------------------------------------

BG_TOP = (34, 38, 52)
BG_BOTTOM = (12, 12, 16)
AMBIENT = 0.28
LIGHT = _norm((0.42, -0.5, 0.75))

# Material table (name + the ``+0x2c`` intensity the loader fills in at
# 0x4214d8..0x421549); index 0..5.  The engine sends a polygon to the second
# (blended) pass when the material entry's first dword has bit 0 set
# (0x437fba / 0x439e75 / 0x43d568).
MATERIALS = ("none", "reflective", "translucent", "varadd", "add", "shade")
MATERIAL_INTENSITY = (0, 64, 64, 100, 127, -128)
BRIGHTNESS_GAIN = 0.5          # poly+0x70 (0..127) -> additive light term


def face_blend(face, materials: bool = True):
    """``(mode, weight)`` for the blend pass, or ``None`` for opaque faces.

    The weights are the engine's own ``material+0x2c`` values; the exact
    per-material blend maths was not traced, so ``translucent`` is mixed at
    half strength and the rest are added (negative weight = subtractive).
    """
    if not materials:
        return None
    idx = None
    if hasattr(face, "material"):
        idx = face.material
    else:
        flags = getattr(face, "material_flags", None) or ()
        for name in flags:
            if name in MATERIALS:
                idx = MATERIALS.index(name)
    if not idx or not (0 < idx < len(MATERIALS)) or idx == 0:
        return None
    w = MATERIAL_INTENSITY[idx] / 127.0
    if MATERIALS[idx] == "translucent":
        return ("mix", 0.5)
    return ("add", w)


def _face_brightness(face) -> float:
    b = getattr(face, "brightness", 0)
    return (b / 127.0) * BRIGHTNESS_GAIN if b else 0.0


def _mesh_aabb(mesh) -> tuple[Vec3, Vec3]:
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for v in mesh.vertices:
        for k in range(3):
            lo[k] = min(lo[k], v[k])
            hi[k] = max(hi[k], v[k])
    if lo[0] > hi[0]:
        return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
    return tuple(lo), tuple(hi)


def _clip_near(view: list[Vec3], groups: list[list], near: float):
    """Sutherland-Hodgman clip of a view-space polygon against ``z >= near``.

    ``groups`` holds one per-vertex attribute tuple per attribute (UV,
    lightmap texel).  Interpolation is linear in view space, which is exactly
    what the perspective-correct interpolation below expects.
    """
    out_v: list[Vec3] = []
    out_g: list[list] = [[] for _ in groups]
    n = len(view)
    for i in range(n):
        cur, nxt = view[i], view[(i + 1) % n]
        cin, nin = cur[2] >= near, nxt[2] >= near
        if cin:
            out_v.append(cur)
            for k, g in enumerate(groups):
                out_g[k].append(g[i])
        if cin != nin:
            t = (near - cur[2]) / (nxt[2] - cur[2])
            out_v.append(tuple(cur[j] + (nxt[j] - cur[j]) * t for j in range(3)))
            for k, g in enumerate(groups):
                a, b = g[i], g[(i + 1) % n]
                out_g[k].append(tuple(a[j] + (b[j] - a[j]) * t for j in range(len(a))))
    return out_v, out_g


def _uv_fit(uvs) -> tuple[float, float, float, float]:
    us = [p[0] for p in uvs]
    vs = [p[1] for p in uvs]
    u0, v0 = min(us), min(vs)
    return u0, v0, max(1e-9, max(us) - u0), max(1e-9, max(vs) - v0)


def render(meshes, width: int = 480, height: int | None = None, *, az: float = 35.0,
           el: float = 22.0, zoom: float = 1.0, fov: float = 45.0,
           pool: TexturePool | None = None, lit: bool = True, wire: bool = False,
           solid: bool = True, uv: str = "fit", cull: bool = False,
           color=(180, 180, 190), bg: bool = True, pos=None, look=None,
           clip: float | None = None, materials: bool = True,
           brightness: bool = True, lightmap: bool = True,
           ambient: float = 0.0, sectors: bool = True,
           only_sector: int | None = None, markers=None, lights=None,
           marker_depth: bool = False, mip: int | str = 0,
           filter: str = "nearest", alpha_test: bool = False,
           transparent_index: int | None = 0,
           lod_bias: float = -0.75) -> bytes:
    """Render one mesh or a list of meshes into an RGB PNG.

    ``pos``/``look`` override the auto-fitted camera (for level interiors),
    ``clip`` drops faces farther than that many units from the eye, and
    ``materials`` enables the two-pass material handling (see
    :func:`face_blend`).  ``lightmap`` samples the level's ``.sbc`` lightmaps
    (see :class:`Lightmaps`) and ``ambient`` adds a constant light level.
    ``sectors`` skips whole meshes whose AABB cannot touch the view frustum
    (image-identical, faster); ``only_sector`` keeps a single level sector.

    Texture sampling defaults to the historical viewer behaviour: mip level 0,
    nearest texel, no alpha test.  ``mip`` may be a level number or ``"auto"``
    (screen-space LOD with ``lod_bias``, the Glide path uses -0.75), ``filter``
    ``"bilinear"`` selects bilinear texels, and ``alpha_test`` drops texels
    equal to ``transparent_index`` (the game keys palette index 0).  All of
    these are opt-in, so the old call still produces the same bytes.

    ``markers`` is a list of ``(pos, rgb)`` scripted scene entities (lights,
    locations, items, sound sources -- see :mod:`gm.scene`) drawn on top of the
    geometry with the drop-line style of an editor overlay; ``lights`` is a list
    of ``(pos, rgb, power)`` sources used to shade the polygons that have *no*
    lightmap (see :func:`_scene_shade`).  Both take coordinates in the frame the
    meshes are in (``gm.scene.Scene.scaled``/``markers`` do the conversion).
    """
    if height is None:
        height = width
    if isinstance(meshes, sobmod.Sob):
        meshes = [meshes]
    meshes = list(meshes)
    if only_sector is not None:
        meshes = [m for m in meshes if getattr(m, "sector", None) == only_sector]
    center, radius = _bounds(meshes)
    cam = Camera(center, radius, width, height, az, el, zoom, fov, pos, look)
    eye = cam.eye

    boxes = [_mesh_aabb(m) for m in meshes]
    if sectors:
        keep = []
        for m, (lo, hi) in zip(meshes, boxes):
            if cam.world_aabb_visible(lo, hi):
                keep.append(m)
        meshes = keep
        boxes = [_mesh_aabb(m) for m in meshes]

    buf = bytearray(width * height * 3)
    if bg:
        for y in range(height):
            t = y / max(1, height - 1)
            row = bytes(int(BG_TOP[k] + (BG_BOTTOM[k] - BG_TOP[k]) * t) for k in range(3))
            buf[y * width * 3 : (y + 1) * width * 3] = row * width

    zbuf = [-1.0] * (width * height)
    blended: list[tuple[float, object, object, list, object]] = []

    # The old call (mip 0 / nearest / no keying) keeps the exact historical
    # code path; the optional sampler only runs when a new mode is requested.
    filt = ("bilinear" if filter in ("bilinear", "linear", "smooth")
            else "nearest")
    sampling = None
    if mip != 0 or filt != "nearest" or alpha_test:
        sampling = (mip, filt, bool(alpha_test), transparent_index, lod_bias)

    for mesh in meshes:
        lms = getattr(mesh, "lightmaps", None) if lightmap else None
        for face in mesh.faces:
            idx = face.indices
            if len(idx) < 3:
                continue
            blend = face_blend(face, materials)
            view = [cam.view(mesh.vertices[i]) for i in idx]
            if clip is not None:
                c = _sub(face.origin, eye) if hasattr(face, "origin") else None
                if c is None:
                    vs = [mesh.vertices[i] for i in idx]
                    c = _sub(tuple(sum(v[k] for v in vs) / len(vs) for k in range(3)), eye)
                if _dot(c, c) > clip * clip:
                    continue
            uvs = [mesh.uv(face, mesh.vertices[i]) for i in idx]
            fit = _uv_fit(uvs) if uv == "fit" and len(uvs) > 2 else None
            lmuv = None
            page = None
            base = 0.0
            if lms is not None and getattr(face, "lightmap", -1) >= 0:
                page = lms.page(face)
                if page is not None:
                    lmuv = [lms.uv(face, mesh.vertices[i], mask=False) for i in idx]
                    base = getattr(face, "brightness", 0) + ambient
            if any(v[2] < cam.near for v in view):
                groups = [uvs] + ([lmuv] if lmuv is not None else [])
                view, groups = _clip_near(view, groups, cam.near)
                if len(view) < 3:
                    continue
                uvs = groups[0]
                lmuv = groups[1] if lmuv is not None else None
            pts = [cam.project_view(v) for v in view]
            if any(p is None for p in pts):
                continue
            lm = (page, lmuv, base) if lmuv is not None else None
            shade = _shade(face, lit)
            if brightness and lm is None:
                shade += _face_brightness(face)
            if lights and lm is None:
                vs = [mesh.vertices[i] for i in idx]
                centre = tuple(sum(v[k] for v in vs) / len(vs) for k in range(3))
                shade = 0.25 + _scene_shade(centre, _face_normal(face), lights)
            col = _face_color(face, tex := (pool.get(face.texture) if pool is not None
                                            else None), lit)
            texobj = None
            if sampling is not None and pool is not None and tex is not None:
                texobj = pool.get_texture(face.texture)
            if blend is not None:
                vs = [mesh.vertices[i] for i in idx]
                c = tuple(sum(v[k] for v in vs) / len(vs) for k in range(3))
                d = _sub(c, eye)
                blended.append((_dot(d, d), face, pts, uvs,
                                (tex, shade, col, blend, uv, cull, lm, fit,
                                 texobj, sampling)))
                continue
            if solid:
                _draw_poly(buf, zbuf, width, height, pts, uvs, tex, shade, col,
                           uv=uv, cull=cull, lm=lm, fit=fit,
                           texobj=texobj, sampling=sampling)
            if wire:
                for k in range(len(pts)):
                    a, b = pts[k], pts[(k + 1) % len(pts)]
                    _line(buf, width, height, a, b, (16, 16, 20))

    # second pass: blended materials, back to front, z-tested but not z-written
    if solid and blended:
        blended.sort(key=lambda t: -t[0])
        for _d, _face, pts, uvs, (tex, sh, col, blend, uv_, cull_, lm_, fit_,
                                  texobj_, sampling_) in blended:
            _draw_poly(buf, zbuf, width, height, pts, uvs, tex, sh, col,
                       uv=uv_, cull=cull_, blend=blend, lm=lm_, fit=fit_,
                       texobj=texobj_, sampling=sampling_)

    # scene overlay: scripted lights/locations/items/sounds (gm.scene)
    if markers:
        for pos, rgb in markers:
            p = cam.project(pos)
            if p is None:
                continue
            _draw_marker(buf, zbuf, width, height, int(p[0]), int(p[1]), p[2], rgb,
                         test=marker_depth)

    return png.rgb(width, height, bytes(buf))


def _face_normal(face) -> Vec3:
    """The face's polygon normal (derived from its UV axes when it is zero)."""
    n = face.normal
    if abs(n[0]) + abs(n[1]) + abs(n[2]) < 1e-9:
        n = _cross(face.u, face.v)
    return _norm(n)


def _shade(face, lit: bool) -> float:
    if not lit:
        return 1.0
    n = face.normal
    if abs(n[0]) + abs(n[1]) + abs(n[2]) < 1e-9:
        n = _norm(_cross(face.u, face.v))
    lam = abs(_dot(_norm(n), LIGHT))
    return AMBIENT + (1.0 - AMBIENT) * lam


def _face_color(face, tex, lit: bool):
    if tex is not None:
        return None
    h = 2166136261
    for ch in face.texture or "none":
        h = ((h ^ ord(ch)) * 16777619) & 0xFFFFFFFF
    r = 110 + (h & 0x3F)
    g = 110 + ((h >> 6) & 0x3F)
    b = 120 + ((h >> 12) & 0x3F)
    return (r, g, b)


def _draw_poly(buf, zbuf, width, height, pts, uvs, tex, shade, col, uv="fit",
               cull=False, blend=None, lm=None, fit=None, texobj=None,
               sampling=None):
    """Triangulate the (convex) face as a fan and rasterise the triangles."""
    if cull:
        area = ((pts[1][0] - pts[0][0]) * (pts[2][1] - pts[0][1])
                - (pts[2][0] - pts[0][0]) * (pts[1][1] - pts[0][1]))
        if area >= 0:
            return
    # per-face UV extent -> [0,1); ``fit`` keeps it stable across near clipping
    u0 = v0 = 0.0
    su = sv = 1.0
    if fit is not None:
        u0, v0, su, sv = fit
    elif uv == "fit" and len(uvs) > 2:
        u0, v0, su, sv = _uv_fit(uvs)
    z_write = blend is None
    for k in range(1, len(pts) - 1):
        tri = (pts[0], pts[k], pts[k + 1])
        tuv = (uvs[0], uvs[k], uvs[k + 1])
        tlm = (lm[0], (lm[1][0], lm[1][k], lm[1][k + 1]), lm[2]) if lm is not None else None
        _triangle(buf, zbuf, width, height, tri, tuv, tex, shade, col,
                  u0, v0, su, sv, blend, z_write, tlm, texobj, sampling)


def _triangle_lod(tuv, tri, texobj, u0, v0, su, sv, bias: float) -> float:
    """Screen-space LOD estimate for one triangle (base-2 mip coordinate).

    Each triangle edge gives ``texels / pixels``; the largest ratio wins, so
    the result is conservative (never sharper than the coarsest edge).  This is
    the CPU analogue of the Glide LOD computation; see ``docs/MIPMAPS.md``.
    """
    lw, lh = texobj.size(0)
    s = []
    for k in range(3):
        sx, sy = tri[k][0], tri[k][1]
        tu = (tuv[k][0] - u0) / su * lw
        tv = (tuv[k][1] - v0) / sv * lh
        s.append((sx, sy, tu, tv))
    g = 0.0
    for a, b in ((0, 1), (1, 2), (2, 0)):
        ds = math.hypot(s[b][0] - s[a][0], s[b][1] - s[a][1])
        dt = math.hypot(s[b][2] - s[a][2], s[b][3] - s[a][3])
        if ds > 1e-9:
            g = max(g, dt / ds)
    if g <= 1e-12:
        return 0.0
    return math.log(g, 2.0) + bias


def _triangle(buf, zbuf, width, height, tri, tuv, tex, shade, col, u0, v0, su, sv,
              blend=None, z_write=True, lm=None, texobj=None, sampling=None):
    (x0, y0, w0, z0), (x1, y1, w1, z1), (x2, y2, w2, z2) = tri
    minx = max(0, int(min(x0, x1, x2)))
    maxx = min(width - 1, int(max(x0, x1, x2)) + 1)
    miny = max(0, int(min(y0, y1, y2)))
    maxy = min(height - 1, int(max(y0, y1, y2)) + 1)
    if maxx < minx or maxy < miny:
        return
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(area) < 1e-9:
        return
    inv = 1.0 / area
    tex_w = tex[0] if tex else 1
    tex_h = tex[1] if tex else 1
    tpx = tex[2] if tex else b""

    # optional sampler: explicit/auto mip level, bilinear filter, alpha key
    sampler = None
    if sampling is not None and tex is not None and texobj is not None:
        mode, filt, alpha, key, bias = sampling
        if isinstance(mode, str):
            lod = _triangle_lod(tuv, tri, texobj, u0, v0, su, sv, bias)
            level = int(math.floor(lod + 0.5))
        else:
            level = int(mode)
        level = max(0, min(level, texobj.levels - 1))
        lw, lh, lpx = texobj.rgb(level)
        sampler = (lw, lh, lpx, texobj.indices(level), filt,
                   bool(alpha) and key is not None, 0 if key is None else int(key))

    mode = blend[0] if blend is not None else None
    bw = blend[1] if blend is not None else 0.0
    lm_page = lm[0] if lm is not None else None
    lm_uv = lm[1] if lm is not None else None
    lm_base = lm[2] if lm is not None else 0.0
    lm_w = lm_page.width if lm_page is not None else 1
    lm_h = lm_page.height if lm_page is not None else 1
    lm_px = lm_page.pixels if lm_page is not None else b""
    for y in range(miny, maxy + 1):
        py = y + 0.5
        for x in range(minx, maxx + 1):
            px = x + 0.5
            l0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv
            if l0 < 0:
                continue
            l1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv
            if l1 < 0:
                continue
            l2 = 1.0 - l0 - l1
            if l2 < 0:
                continue
            i = y * width + x
            iz = l0 * w0 + l1 * w1 + l2 * w2
            if iz <= zbuf[i]:
                continue
            transparent = False
            if tex is None:
                r, g, b = col
            elif sampler is None:
                # perspective-correct UVs
                uu = (l0 * tuv[0][0] * w0 + l1 * tuv[1][0] * w1 + l2 * tuv[2][0] * w2) / iz
                vv = (l0 * tuv[0][1] * w0 + l1 * tuv[1][1] * w1 + l2 * tuv[2][1] * w2) / iz
                tx = int((uu - u0) / su * tex_w) % tex_w
                ty = int((vv - v0) / sv * tex_h) % tex_h
                o = (ty * tex_w + tx) * 3
                r, g, b = tpx[o], tpx[o + 1], tpx[o + 2]
            else:
                lw, lh, lpx, lidx, filt, alpha, key = sampler
                uu = (l0 * tuv[0][0] * w0 + l1 * tuv[1][0] * w1 + l2 * tuv[2][0] * w2) / iz
                vv = (l0 * tuv[0][1] * w0 + l1 * tuv[1][1] * w1 + l2 * tuv[2][1] * w2) / iz
                fu = (uu - u0) / su * lw
                fv = (vv - v0) / sv * lh
                if filt == "nearest":
                    tx = int(math.floor(fu)) % lw
                    ty = int(math.floor(fv)) % lh
                    o = (ty * lw + tx) * 3
                    r, g, b = lpx[o], lpx[o + 1], lpx[o + 2]
                    if alpha and lidx[ty * lw + tx] == key:
                        transparent = True
                else:
                    fu -= 0.5
                    fv -= 0.5
                    tx0 = int(math.floor(fu))
                    ty0 = int(math.floor(fv))
                    ax = fu - tx0
                    ay = fv - ty0
                    tx0 %= lw
                    ty0 %= lh
                    tx1 = (tx0 + 1) % lw
                    ty1 = (ty0 + 1) % lh
                    o00 = (ty0 * lw + tx0) * 3
                    o10 = (ty0 * lw + tx1) * 3
                    o01 = (ty1 * lw + tx0) * 3
                    o11 = (ty1 * lw + tx1) * 3
                    w00 = (1.0 - ax) * (1.0 - ay)
                    w10 = ax * (1.0 - ay)
                    w01 = (1.0 - ax) * ay
                    w11 = ax * ay
                    r = (lpx[o00] * w00 + lpx[o10] * w10
                         + lpx[o01] * w01 + lpx[o11] * w11)
                    g = (lpx[o00 + 1] * w00 + lpx[o10 + 1] * w10
                         + lpx[o01 + 1] * w01 + lpx[o11 + 1] * w11)
                    b = (lpx[o00 + 2] * w00 + lpx[o10 + 2] * w10
                         + lpx[o01 + 2] * w01 + lpx[o11 + 2] * w11)
                    if alpha:
                        a = (w00 * (lidx[ty0 * lw + tx0] != key)
                             + w10 * (lidx[ty0 * lw + tx1] != key)
                             + w01 * (lidx[ty1 * lw + tx0] != key)
                             + w11 * (lidx[ty1 * lw + tx1] != key))
                        transparent = a <= 0.5
            if transparent:                 # keyed texel: no colour, no depth
                continue
            if z_write:
                zbuf[i] = iz
            if lm_page is not None:
                # lightmap sample: same affine projection as the engine (0x43caxx)
                lu = (l0 * lm_uv[0][0] * w0 + l1 * lm_uv[1][0] * w1 + l2 * lm_uv[2][0] * w2) / iz
                lv = (l0 * lm_uv[0][1] * w0 + l1 * lm_uv[1][1] * w1 + l2 * lm_uv[2][1] * w2) / iz
                light = lm_base + lm_px[(int(lv) & (lm_h - 1)) * lm_w + (int(lu) & (lm_w - 1))]
                f = (127.0 if light > 127 else (0.0 if light < 0 else light)) / 127.0
            else:
                f = shade
            r, g, b = r * f, g * f, b * f
            o2 = i * 3
            if mode == "add":
                buf[o2] = _clamp(buf[o2] + r * bw)
                buf[o2 + 1] = _clamp(buf[o2 + 1] + g * bw)
                buf[o2 + 2] = _clamp(buf[o2 + 2] + b * bw)
            elif mode == "mix":
                buf[o2] = _clamp(buf[o2] * (1.0 - bw) + r * bw)
                buf[o2 + 1] = _clamp(buf[o2 + 1] * (1.0 - bw) + g * bw)
                buf[o2 + 2] = _clamp(buf[o2 + 2] * (1.0 - bw) + b * bw)
            else:
                buf[o2] = _clamp(r)
                buf[o2 + 1] = _clamp(g)
                buf[o2 + 2] = _clamp(b)


def _clamp(v: float) -> int:
    return 0 if v < 0 else (255 if v > 255 else int(v))


def _scene_shade(centre, normal, lights, base: float = 0.0) -> float:
    """Additive Lambert light from the scripted light sources.

    This is the viewer's approximation of what the engine bakes into the
    lightmaps (``0x43e69d``, fed from the sky/local light arrays at ``0x638130``
    / ``0x638330``): a cosine term times a soft inverse-square falloff with a
    300-unit reference distance.  Polygons that *have* a lightmap keep using it.
    """
    if not lights:
        return base
    n = normal
    if abs(n[0]) + abs(n[1]) + abs(n[2]) < 1e-9:
        n = (0.0, 0.0, 1.0)
    else:
        n = _norm(n)
    out = base
    for p, _rgb, power in lights:
        d = (p[0] - centre[0], p[1] - centre[1], p[2] - centre[2])
        dist = math.sqrt(_dot(d, d))
        if dist < 1e-6:
            continue
        lam = _dot(n, (d[0] / dist, d[1] / dist, d[2] / dist))
        if lam <= 0.0:
            continue
        out += power * lam / (1.0 + (dist / 300.0) ** 2)
    return out


def _draw_marker(buf, zbuf, width, height, sx, sy, depth, rgb, size: int = 3,
                 tick: int = 14, test: bool = False) -> None:
    """Draw a small square with a vertical drop line.

    ``test`` also depth-tests every pixel against ``zbuf`` (so a marker hidden
    behind geometry disappears); the default draws the overlay on top, the way
    an editor's scene view does.
    """
    def hidden(idx):
        return test and zbuf[idx] > depth

    x0, x1 = max(0, sx - size), min(width - 1, sx + size)
    y0, y1 = max(0, sy - size), min(height - 1, sy + size)
    for y in range(y0, y1 + 1):
        row = y * width
        for x in range(x0, x1 + 1):
            if hidden(row + x):
                continue
            p = (row + x) * 3
            buf[p : p + 3] = bytes(rgb)
    col = bytes(int(c * 0.55) for c in rgb)
    for y in range(y1 + 1, min(height, y1 + 1 + tick)):
        row = y * width
        if hidden(row + sx):
            break
        p = (row + sx) * 3
        buf[p : p + 3] = col


def _line(buf, width, height, a, b, rgb):
    x0, y0 = a[0], a[1]
    x1, y1 = b[0], b[1]
    n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
    for s in range(n + 1):
        t = s / n
        x = int(x0 + (x1 - x0) * t)
        y = int(y0 + (y1 - y0) * t)
        if 0 <= x < width and 0 <= y < height:
            o = (y * width + x) * 3
            buf[o], buf[o + 1], buf[o + 2] = rgb


# ---------------------------------------------------------------------------
# small helpers used by the CLI and the tests
# ---------------------------------------------------------------------------


SHEET_ANGLES = (("iso", 35.0, 22.0), ("front", 0.0, 0.0),
                ("left", 90.0, 0.0), ("top", 0.0, 89.0))


def contact_sheet(meshes, size: int = 480, *, pool: TexturePool | None = None,
                  angles=SHEET_ANGLES, gap: int = 2, **kw) -> bytes:
    """Render the same mesh from several angles into a 2x2-styled sheet."""
    if isinstance(meshes, sobmod.Sob):
        meshes = [meshes]
    tiles = []
    tile = max(16, (size - gap) // 2)
    for _name, az, el in angles:
        tiles.append(png.decode(render(meshes, tile, az=az, el=el, pool=pool, **kw)))
    cols = 2
    rows = (len(tiles) + cols - 1) // cols
    width = cols * tile + (cols - 1) * gap
    height = rows * tile + (rows - 1) * gap
    buf = bytearray(width * height * 3)
    for k, img in enumerate(tiles):
        ox = (k % cols) * (tile + gap)
        oy = (k // cols) * (tile + gap)
        for y in range(img.height):
            src = y * img.width * img.channels
            dst = ((oy + y) * width + ox) * 3
            row = img.pixels[src : src + img.width * img.channels]
            if img.channels == 3:
                buf[dst : dst + img.width * 3] = row
            else:
                for x in range(img.width):
                    buf[dst + x * 3 : dst + x * 3 + 3] = row[x * img.channels : x * img.channels + 3]
    return png.rgb(width, height, bytes(buf))


def ascii_preview(data: bytes, cols: int = 78) -> str:
    """Coarse ASCII view of a rendered PNG (for tests and terminals)."""
    img = png.decode(data)
    ramp = " .:-=+*#%@"
    step = max(1, img.width // cols)
    lines = []
    for y in range(0, img.height, max(1, step * 2)):
        row = []
        for x in range(0, img.width, step):
            o = (y * img.width + x) * img.channels
            if img.channels >= 3:
                v = (img.pixels[o] * 3 + img.pixels[o + 1] * 6 + img.pixels[o + 2]) // 10
            else:
                v = img.pixels[o]
            row.append(ramp[min(len(ramp) - 1, v * len(ramp) // 256)])
        lines.append("".join(row))
    return "\n".join(lines)


def coverage(data: bytes) -> float:
    """Fraction of pixels that differ from the background gradient."""
    img = png.decode(data)
    n = 0
    total = img.width * img.height
    for y in range(img.height):
        t = y / max(1, img.height - 1)
        ref = tuple(int(BG_TOP[k] + (BG_BOTTOM[k] - BG_TOP[k]) * t) for k in range(3))
        for x in range(img.width):
            o = (y * img.width + x) * img.channels
            if img.channels >= 3:
                if abs(img.pixels[o] - ref[0]) + abs(img.pixels[o + 1] - ref[1]) + \
                        abs(img.pixels[o + 2] - ref[2]) > 12:
                    n += 1
    return n / total
