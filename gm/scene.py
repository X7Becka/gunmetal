"""Script-driven level scene: lights and placed objects the SCRIPT block builds.

The shipped ``.sbt`` blocks contain **no** pre-placed object list: the
``SCRIPT`` record's ``things`` array is empty in all 36 levels and no script
emits an ``ADD_THING*`` command.  The scene is built at run time by the compiled
code, so the code itself is the only static source for it.

This module decodes that code (see :mod:`gm.sbtvm`) and pulls out the placement
commands whose operand layout is proven from the engine handlers:

``ADD_SKY_LIGHT_SOURCE`` (op 88, ``LLLLLLL``) -- "sky" light
    Handler ``0x47e130`` (dispatcher ``0x45c500`` is indexed by ``op - 1``)
    writes ``pos = (double)a0, (double)a1, (double)a2`` (``fild``) into the sky
    light array ``0x638130`` (stride 0x20, at most 16) and then stores ``a3`` at
    ``+0x18`` plus the low *bytes* of ``a4``, ``a5``, ``a6`` at ``+0x1c/+0x1d/
    +0x1e`` -- an intensity/flags word and a colour.

``ADD_LIGHT_SOURCE`` (op 87, 16 x ``L``) -- "local" light
    Handler ``0x47e1b0``: same ``fild`` triple at the head into the local light
    array ``0x638330`` (stride 0x50, at most 1000), the same colour byte triple
    from ``a4/a5/a6``, a direction vector from ``a9/a10/a11`` and further
    size/range words.

``ADD_LOCATION`` (op 196, ``ELLLAAA``) -- placed thing / named location
    Handler ``0x45bea6`` appends the ten operands verbatim to the engine's
    "thing" array (``0x80d8e0``, stride 0x28, count ``0x80d87c``); the consumer
    ``0x476008`` reads ``x = (int)+0x04, y = +0x08, z = +0x0c`` with ``fild``.
    So the position is the 2nd..4th operands and the three doubles are an
    orientation (``level1`` stores yaw = pi).

``ADD_ITEM`` / ``ADD_ITEM_FIXED`` (ops 53/55, ``OPPEELLLLAAAP``)
    Handler forwards all 16 dwords to ``0x4686a0``, which fills a free slot of
    the 300-entry item array at ``0x806398`` (stride 0x60) and takes the
    position from ``fild [esp+0x18]/[esp+0x1c]/[esp+0x20]`` -- i.e. operands
    5, 6 and 7.

``ADD_SOUND_SOURCE`` (op 187, ``LLLNVD``) -- sound emitter
    Handler ``0x459c6e`` forwards the seven operands to ``0x46b210``, which
    stores ``a0`` at ``0x800340``, ``a1`` at ``0x800540``, ``a2`` at
    ``0x800140``, ``a3`` at ``0x800540+`` and the trailing double at
    ``0x800760`` (parallel 128-element arrays).  Which of the three integer
    arrays is x/y/z is not proven from the code; the first three operands are
    taken as the position because they behave like one (see the frame note).

``ADD_SPRITE`` (op 66, ``RDDDLLLLMDDL``) -- a billboard/model at a literal spot
    Handler ``0x4592c2`` reads the ``DDD`` triple at ``+0x04/+0x0c/+0x14`` and
    calls ``0x40e0e0`` after adding a per-axis random offset bounded by ``a8``
    (``idiv (2*a8+1)``), i.e. the triple is the sprite's position and ``a8`` its
    placement jitter.  ``ADD_SPRITE_AT_SOURCE`` never carries a position (it is
    taken from the source object), so only ``ADD_SPRITE`` can be placed
    statically.

Coordinate frame
----------------
Script coordinates are **engine world** coordinates, and the engine scales every
``.sbw`` object by the diagonal basis of its 96-byte header (``1.875``) at load
(``object.points[0]`` is the origin).  That is, ``world = origin + 1.875 * v``
for the file's vertex ``v``, while the renderer and every lightmap projection
work in the raw file frame.  ``SCALE`` below is the factor that maps world
coordinates into the file frame.

The frame is supported by a scale-covariant argument: sprite positions are props
that must sit **inside** their level, so over one level the sprite spread cannot
exceed the geometry spread of that same level.  Measured over 23 levels (levels
with at least four ``ADD_SPRITE`` commands) the median ratio between the two
spreads is **1.52** (1.88 if levels with three sprites are included) -- i.e. the
script coordinates are ~1.5-1.9x the raw file frame, which only the 1.875 basis
explains (a same-frame reading would have to come out at <= 1).  The earlier
"fraction of positions inside the level AABB" test points the same way but is
biased (shrinking the positions towards the origin raises it monotonically), and
so is "distance to the nearest vertex"; the baked lightmaps do not correlate
with the light positions at all (|r| < 0.18), and the ``ADD_LIGHT_SOURCE`` /
``ADD_SOUND_SOURCE`` operands do not fit this frame either (their spread is ~5x
the level's), so those two positions stay unproven: use them for markers only or
tune ``--scene-scale``.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from . import sbt, sbtvm

#: Engine world -> raw ``.sbw`` frame (the object header basis).
SCALE = 1.0 / 1.875


def _s32(x: int) -> int:
    """Interpret a decoded 4-byte operand as the signed int the engine uses."""
    return x - 0x100000000 if x >= 0x80000000 else x


def _op(name: str) -> int:
    for k, v in sbtvm.COMMANDS.items():
        if v[0] == name:
            return k
    raise KeyError(name)


@dataclass
class Light:
    """A light source placed by the script (world coordinates)."""

    pos: tuple
    rgb: tuple = (255, 255, 255)
    kind: str = "local"  # "sky" | "local"
    param: int = 0  # a3: intensity/flags word (opaque)
    extra: tuple = ()  # direction/size words, unproven roles

    @property
    def strength(self) -> float:
        return 1.0


@dataclass
class Location:
    """``ADD_LOCATION``: a placed thing / named location."""

    thing_id: int
    pos: tuple
    orientation: tuple = (0.0, 0.0, 0.0)

    @property
    def yaw(self) -> float:
        return self.orientation[2]


@dataclass
class Item:
    """``ADD_ITEM`` / ``ADD_ITEM_FIXED``: a pickup placed by the script."""

    item_id: int
    pos: tuple
    fixed: bool = False
    extra: tuple = ()


@dataclass
class SoundSource:
    """``ADD_SOUND_SOURCE``: a sound emitter (position per the frame note)."""

    pos: tuple
    sample: int = 0
    volume: int = 0
    pitch: float = 0.0


@dataclass
class Sprite:
    """``ADD_SPRITE``: a billboard/model at a literal position."""

    pos: tuple
    jitter: int = 0  # a8: spawn scatter radius (rand in [-a8, a8] per axis)
    id: int = 0  # a0
    extra: tuple = ()


@dataclass
class Scene:
    lights: list[Light] = field(default_factory=list)
    locations: list[Location] = field(default_factory=list)
    items: list[Item] = field(default_factory=list)
    sounds: list[SoundSource] = field(default_factory=list)
    sprites: list[Sprite] = field(default_factory=list)
    #: commands seen whose operand layout is still opaque (name -> count)
    other: dict = field(default_factory=dict)

    def counts(self) -> dict:
        return {"lights": len(self.lights), "locations": len(self.locations),
                "items": len(self.items), "sounds": len(self.sounds),
                "sprites": len(self.sprites), "other": sum(self.other.values())}

    def scaled(self, factor: float = SCALE) -> Scene:
        """A copy with every position multiplied by ``factor``."""

        def s(p):
            return tuple(v * factor for v in p)

        return Scene(
            [Light(s(l.pos), l.rgb, l.kind, l.param, l.extra) for l in self.lights],
            [Location(x.thing_id, s(x.pos), x.orientation) for x in self.locations],
            [Item(x.item_id, s(x.pos), x.fixed, x.extra) for x in self.items],
            [SoundSource(s(x.pos), x.sample, x.volume, x.pitch) for x in self.sounds],
            [Sprite(s(x.pos), x.jitter, x.id, x.extra) for x in self.sprites],
            dict(self.other),
        )

    def markers(self, scale: float = SCALE):
        """``(pos, rgb)`` triples ready for :func:`gm.render.render`."""
        out = []
        for l in self.lights:
            col = l.rgb if l.kind == "sky" else tuple(min(255, c) for c in l.rgb)
            out.append((tuple(v * scale for v in l.pos), col))
        for x in self.sprites:
            out.append((tuple(v * scale for v in x.pos), (255, 150, 60)))
        for x in self.locations:
            out.append((tuple(v * scale for v in x.pos), (255, 240, 90)))
        for x in self.items:
            out.append((tuple(v * scale for v in x.pos), (100, 220, 255)))
        for x in self.sounds:
            out.append((tuple(v * scale for v in x.pos), (150, 150, 160)))
        return out

    def render_lights(self, scale: float = SCALE):
        """``(pos, rgb, power)`` triples ready for :func:`gm.render.render`."""
        out = []
        for l in self.lights:
            power = 1.6 if l.kind == "sky" else 1.0
            out.append((tuple(v * scale for v in l.pos), l.rgb, power))
        return out


#: opcode -> handler; only the layouts proven above are decoded.
_SKY = _op("ADD_SKY_LIGHT_SOURCE")
_LOCAL = _op("ADD_LIGHT_SOURCE")
_LOCATION = _op("ADD_LOCATION")
_ITEM = _op("ADD_ITEM")
_ITEM_FIXED = _op("ADD_ITEM_FIXED")
_SOUND = _op("ADD_SOUND_SOURCE")
_SPRITE = _op("ADD_SPRITE")

#: placement commands that are seen but not decoded (opaque operand layout)
_SEEN = ("ADD_NAMED_ITEM", "ADD_NAMED_ITEM_EX", "ADD_SWITCH", "ADD_DOOR",
         "ADD_SPRITE_AT_SOURCE", "ADD_SPRITE_MOVING", "ADD_SPRITE_MOVING_AT_SOURCE",
         "ADD_ITEM_AT_SOURCE", "ADD_ITEM_AT_SOURCE_FIXED",
         "ADD_WEAPON_AT_SOURCE")


def _walk(insns):
    """Every instruction, recursing into ``IF``/``ELSE`` bodies."""
    for i in insns:
        yield i
        if isinstance(getattr(i, "body", None), list):
            yield from _walk(i.body)


def extract(script: sbt.Script) -> Scene:
    """Decode a ``SCRIPT`` record into its lights and placed objects."""
    ins, _end = sbtvm.decode(script.code)
    scene = Scene()
    for i in _walk(ins):
        op = getattr(i, "op", None)
        if op not in (_SKY, _LOCAL, _LOCATION, _ITEM, _ITEM_FIXED, _SOUND, _SPRITE):
            if isinstance(i.name, str) and i.name in _SEEN:
                scene.other[i.name] = scene.other.get(i.name, 0) + 1
            continue
        a = i.args
        if op == _SKY and len(a) >= 7:
            x, y, z = (_s32(a[k]) for k in range(3))
            scene.lights.append(Light((x, y, z),
                                      tuple(a[k] & 0xFF for k in (4, 5, 6)),
                                      "sky", _s32(a[3])))
        elif op == _LOCAL and len(a) >= 3:
            x, y, z = (_s32(a[k]) for k in range(3))
            rgb = tuple(a[k] & 0xFF for k in (4, 5, 6)) if len(a) >= 7 else (255,) * 3
            extra = tuple(_s32(v) for v in a[9:12]) if len(a) >= 12 else ()
            scene.lights.append(Light((x, y, z), rgb, "local",
                                      _s32(a[3]) if len(a) > 3 else 0, extra))
        elif op == _LOCATION and len(a) >= 7:
            x, y, z = (_s32(a[k]) for k in (1, 2, 3))
            scene.locations.append(Location(_s32(a[0]), (x, y, z), tuple(a[4:7])))
        elif op in (_ITEM, _ITEM_FIXED) and len(a) >= 8:
            x, y, z = (_s32(a[k]) for k in (5, 6, 7))
            scene.items.append(Item(_s32(a[1]), (x, y, z), op == _ITEM_FIXED,
                                    tuple(_s32(v) for v in a[:5])))
        elif op == _SOUND and len(a) >= 3:
            x, y, z = (_s32(a[k]) for k in range(3))
            scene.sounds.append(SoundSource((x, y, z),
                                            _s32(a[3]) if len(a) > 3 else 0,
                                            _s32(a[4]) if len(a) > 4 else 0,
                                            float(a[5]) if len(a) > 5 else 0.0))
        elif op == _SPRITE and len(a) >= 9:
            # handler 0x4592c2 scatters DDD by a random offset of +-a8 per axis
            scene.sprites.append(Sprite((float(a[1]), float(a[2]), float(a[3])),
                                        _s32(a[8]), _s32(a[0]),
                                        tuple(_s32(v) for v in a[4:9])))
    return scene


def from_sbt(path_or_file) -> Scene:
    """``Scene`` of a parsed :class:`gm.sbt.Sbt` or of an ``.sbt`` path."""
    f = path_or_file
    if isinstance(f, str):
        f = sbt.parse_file(f)
    recs = f.records_of(11)
    if not recs:
        raise ValueError("no SCRIPT record")
    return extract(recs[0].payload)


def load(path: str) -> Scene:
    """Scene of an ``.sbt`` file, in the engine's world coordinates."""
    return from_sbt(sbt.parse_file(path))
