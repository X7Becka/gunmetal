"""Script-driven level scene: lights and placed objects the SCRIPT block builds.

The shipped ``.sbt`` blocks contain **no** pre-placed object list: the
``SCRIPT`` record's ``things`` array is empty in all 36 levels, and the world is
built at run time by the compiled code -- which is therefore the only static
source for the scene.  Things are *not* placed by commands, but by the
``ADD_THING*`` family of **conditions** (ids 1..5), which the entity sub-scripts
evaluate; see :func:`things` and ``docs/FORMATS.md`` 12.4.

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
class Thing:
    """A thing built by an ``ADD_THING*`` condition inside an entity's sub-script.

    The type is the condition's ``G`` operand, an index into ``THING_TYPES``
    (all 4005 real ones in the shipped levels are valid; three are
    ``0xFFFFFFFF`` = "none").  The handler of the family calls the thing builder
    ``0x46d000(type, ...)``, which reads ``fild`` of the three *integer*
    operands for the position and the doubles for an orientation -- the same
    convention as ``ADD_LOCATION``.  The ``AT_SOURCE``/``RELATIVE`` variants take
    the position from a (moving) source, so only ``ADD_THING`` and
    ``ADD_THING_EX`` carry an absolute ``pos`` here.
    """

    entity: int          # id of the entity whose sub-script builds it
    kind: str            # the ADD_THING* condition name
    type_index: int
    type_name: str = ""
    model: str = ""
    pos: tuple | None = None   # world coordinates, None when source-relative
    angles: tuple = ()
    owner: int = 0       # the L operand in front of the type (player/source)

    @property
    def absolute(self) -> bool:
        return self.pos is not None


@dataclass
class Door:
    """``ADD_DOOR``: the world editor's door record.

    The handler (``0x459b5b``) forwards the operands to ``0x44e250``, which
    stores them **permuted** into the door array ``0x8381a0`` (stride ``0xB8``,
    count ``0x5a9450``, at most 100; the ids therefore sit at ``+0x68`` flags,
    ``+0x6c``..``+0x7c`` ids).  ``GENERATE`` finds a door by the id in
    ``0x44f690`` -- it compares ``+0x6c/+0x70/+0x74`` (the first three ``E``
    operands) and toggles the state words at ``+0x80`` and ``+0x8c``.
    """

    flags: int            # operand 1 -> door+0x68 (its low nibble is tested)
    ids: tuple = ()       # the five ``E`` operands -> door+0x6c..+0x7c
    angles: tuple = ()    # the three ``A`` doubles (0 / pi/2 / -pi/2 in data)
    params: tuple = ()    # the seven ``L`` ints (200/300/500/600/700/120/99999)
    values: tuple = ()    # the nine ``D`` doubles (bbox/centre, derived at load)
    tail: tuple = ()      # ``VNVNVN`` (0/1/120/700/1500/99999 in data)

    #: the two state slots ``GENERATE`` toggles (``params[4]``/``params[3]``)
    @property
    def state(self) -> tuple:
        p = self.params
        return (p[4] if len(p) > 4 else 0, p[3] if len(p) > 3 else 0)


@dataclass
class Switch:
    """``ADD_SWITCH``: a trigger, stored **verbatim** in ``0x5a4af0``.

    Handler ``0x45b6ff`` -> ``0x44e5f0`` copies the eight operands as-is (array
    ``0x5a4af0``, stride ``0x20``, count ``0x5a945c``, at most 512).  The
    consumer ``0x44f960`` tests operand 1 against ``0x101``, takes **operand 2 as
    the id** and calls ``0x479960`` (GENERATE) with operands 4/5/6 as the source
    context -- and 86.9% of those triples fall inside the level's core geometry
    box, i.e. operands 4..6 are the trigger's position.
    """

    kind: int             # operand 1 (compared with 0x101)
    id: int               # operand 2: the entity the switch generates
    radius: int = 0       # operand 3 (radius/delay, not proven)
    pos: tuple = ()       # operands 4..6 (verified as a position)
    extra: tuple = ()     # operands 7..8 (0, -1)

    @property
    def absolute(self) -> bool:
        return len(self.pos) == 3


@dataclass
class Lift:
    """``ADD_LIFT`` (structural op 188): ``0x94`` bytes + ``n`` stops of ``0xc``.

    None of the fields are decoded yet -- the payload is kept verbatim (45
    lifts in the whole game, ``level2``/``level17``/... only).
    """

    head: bytes = b""
    stops: tuple = ()


@dataclass
class Scene:
    lights: list[Light] = field(default_factory=list)
    locations: list[Location] = field(default_factory=list)
    items: list[Item] = field(default_factory=list)
    sounds: list[SoundSource] = field(default_factory=list)
    sprites: list[Sprite] = field(default_factory=list)
    #: the level's entity table as ``(id, flags, offset)`` triples
    entities: list = field(default_factory=list)
    #: things built by the entity sub-scripts (``ADD_THING*``)
    things: list = field(default_factory=list)
    #: doors / switches / lifts (``ADD_DOOR``, ``ADD_SWITCH``, ``ADD_LIFT``)
    doors: list = field(default_factory=list)
    switches: list = field(default_factory=list)
    lifts: list = field(default_factory=list)
    #: commands seen whose operand layout is still opaque (name -> count)
    other: dict = field(default_factory=dict)

    def counts(self) -> dict:
        return {"lights": len(self.lights), "locations": len(self.locations),
                "items": len(self.items), "sounds": len(self.sounds),
                "sprites": len(self.sprites), "entities": len(self.entities),
                "things": len(self.things), "doors": len(self.doors),
                "switches": len(self.switches), "lifts": len(self.lifts),
                "other": sum(self.other.values())}

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
            list(self.entities),
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
        for x in self.switches:
            if x.absolute:
                out.append((tuple(v * scale for v in x.pos), (255, 90, 220)))
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
_DOOR = _op("ADD_DOOR")
_SWITCH = _op("ADD_SWITCH")
_LIFT = 188                       # structural keyword, decoded by gm.sbtvm

#: placement commands that are seen but not decoded (opaque operand layout)
_SEEN = ("ADD_NAMED_ITEM", "ADD_NAMED_ITEM_EX",
         "ADD_SPRITE_AT_SOURCE", "ADD_SPRITE_MOVING", "ADD_SPRITE_MOVING_AT_SOURCE",
         "ADD_ITEM_AT_SOURCE", "ADD_ITEM_AT_SOURCE_FIXED",
         "ADD_WEAPON_AT_SOURCE")


def _walk(insns):
    """Every instruction, recursing into ``IF``/``ELSE`` bodies."""
    for i in insns:
        yield i
        if isinstance(getattr(i, "body", None), list):
            yield from _walk(i.body)


def entities(script: sbt.Script) -> list[tuple[int, int, int]]:
    """The level's entity table: ``(id, flags, offset)`` per record.

    The ``SCRIPT`` block keeps three columns of ``n`` u32 right after the things
    array (loader ``0x462b03``..``0x462bb0``: flags -> ``0x5b0df0``, ids ->
    ``0x5e4680``, offsets -> ``0x616658``, the count -> ``0x5b1df0``).  The
    commands address an entity by the *id* (a linear search over the id column,
    e.g. ``0x457c49``/``0x457c80``/``0x457cd5`` setting or clearing bits of the
    flags word), the compiler writes ``0x3ff`` into flags and appends the record
    (``0x460843``..``0x4608a8``, the id coming from the name token the lexer at
    ``0x45fb90``/``0x460ea0`` interns), and the last column is the offset of the
    entity's **sub-script in the compiled code** -- ``GENERATE`` (cid 2) runs it
    through ``0x457390`` (``0x45fa00`` -> ``0x45fb01``), and that sub-script is
    where the entity's thing is built (``ADD_THING*`` -> ``0x46d000``), carrying
    the ``THING_TYPES`` index that ``+0x1a4`` turns into the model.
    """
    n = script.n
    if n <= 0 or len(script.handles) < 12 * n:
        return []
    cols = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
    return [(_s32(cols[n + i]), cols[i], cols[2 * n + i]) for i in range(n)]


def _cond(name: str) -> str:
    for _cid, (nm, sig) in sbtvm.CONDITIONS.items():
        if nm == name:
            return sig or ""
    return ""


#: ``ADD_THING*`` kinds and whether their position is absolute (the others take
#: it from a moving source: ``ADD_THING_AT_SOURCE``, ``..._RELATIVE``, ...)
_THINGS_ABS = ("ADD_THING", "ADD_THING_EX")


def _thing_args(args, sig):
    """Split the operands of an ``ADD_THING*`` by signature letter.

    Returns ``(type_index, ints, doubles, owner)``: the ``G`` operand is the
    thing type, the other 4-byte letters are integers and ``A``/``B``/``D`` are
    doubles, in operand order.  The first integer (if any precedes the type) is
    the owner/source or player index.
    """
    ti = None
    owner = 0
    ints: list = []
    dbls: list = []
    for ch, v in zip(sig, args):
        if ch == "G":
            ti = v
        elif ch in "ABD":
            dbls.append(float(v))
        elif isinstance(v, int):
            if ti is None and not ints:
                owner = _s32(v)
            else:
                ints.append(_s32(v))
    return ti, ints, dbls, owner


def things(script: sbt.Script, sbt_file=None) -> list[Thing]:
    """Every ``ADD_THING*`` built by the level's entity sub-scripts.

    Each entity row of the table names a sub-script (its ``offset``, run by
    ``GENERATE`` through ``0x457390``); decoding that block yields the
    ``ADD_THING*`` conditions that build the entity's thing.  With *sbt_file*
    (a parsed :class:`gm.sbt.Sbt`) the type name and the ``SOB`` model are
    resolved through ``THING_TYPES``/``SOB_TABLE``.
    """
    out: list[Thing] = []
    names: list = []
    models: list = []
    if sbt_file is not None:
        names = [t.name for t in sbt_file.records_of(4)[0].payload.entries]
        models = [e.name for e in sbt_file.records_of(20)[0].payload.entries]
    code = script.code
    for eid, _flags, off in entities(script):
        if not 0 <= off < len(code):
            continue
        try:
            ins, _end = sbtvm.decode(code, off, len(code))
        except Exception:                       # noqa: BLE001 - skip bad block
            continue
        for i in ins:
            if i.name == "END":
                break
            if i.op != 0xC9 or not i.args:      # 0xC9 = EVAL <cond>
                continue
            cname, args = i.args[0]
            if not cname.startswith("ADD_THING"):
                continue
            ti, ints, dbls, owner = _thing_args(args, _cond(cname))
            if ti is None:
                continue
            name = model = ""
            if 0 <= ti < len(names):
                name = names[ti]
                if sbt_file is not None:
                    model = models[_thing_model(sbt_file, ti)]
            pos = tuple(ints[:3]) if (cname in _THINGS_ABS and len(ints) >= 3) else None
            out.append(Thing(eid, cname, _s32(ti), name, model, pos,
                             tuple(dbls), owner))
    return out


def _thing_model(sbt_file, idx: int) -> int:
    """``SOB_TABLE`` index of ``THING_TYPES[idx]`` (its ``+0x1a4`` field)."""
    tt = sbt_file.records_of(4)[0].payload.entries[idx]
    tail = struct.unpack_from("<%dI" % (len(tt.tail) // 4), tt.tail)
    return tail[(0x1A4 - 0x198) // 4]


def extract(script: sbt.Script) -> Scene:
    """Decode a ``SCRIPT`` record into its lights and placed objects."""
    ins, _end = sbtvm.decode(script.code)
    scene = Scene(entities=entities(script))
    for i in _walk(ins):
        op = getattr(i, "op", None)
        if op not in (_SKY, _LOCAL, _LOCATION, _ITEM, _ITEM_FIXED, _SOUND,
                      _SPRITE, _DOOR, _SWITCH, _LIFT):
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
        elif op == _DOOR and len(a) >= 7:
            # handler 0x459b5b -> 0x44e250 (door array 0x8381a0, stride 0xB8)
            scene.doors.append(Door(
                _s32(a[0]),
                tuple(_s32(v) for v in a[1:6]),
                tuple(float(v) for v in a[6:9]),
                tuple(_s32(v) for v in a[9:16]),
                tuple(float(v) for v in a[16:25]),
                tuple(_s32(v) for v in a[25:31])))
        elif op == _SWITCH and len(a) >= 6:
            # handler 0x45b6ff -> 0x44e5f0 (verbatim into 0x5a4af0, stride 0x20)
            scene.switches.append(Switch(
                _s32(a[0]), _s32(a[1]), _s32(a[2]),
                tuple(_s32(v) for v in a[3:6]),
                tuple(_s32(v) for v in a[6:8])))
        elif op == _LIFT:
            data = i.data
            if isinstance(data, tuple) and len(data) == 2:
                head, stops = data
                scene.lifts.append(Lift(bytes(head),
                                        tuple(tuple(r) for r in stops)))
    return scene


def from_sbt(path_or_file) -> Scene:
    """``Scene`` of a parsed :class:`gm.sbt.Sbt` or of an ``.sbt`` path."""
    f = path_or_file
    if isinstance(f, str):
        f = sbt.parse_file(f)
    recs = f.records_of(11)
    if not recs:
        raise ValueError("no SCRIPT record")
    scene = extract(recs[0].payload)
    scene.things = things(recs[0].payload, f)
    return scene


def load(path: str) -> Scene:
    """Scene of an ``.sbt`` file, in the engine's world coordinates."""
    return from_sbt(sbt.parse_file(path))
