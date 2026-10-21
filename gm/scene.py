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
Script coordinates are **engine world** coordinates and the ``.sbw`` header is
``12 x f64``: ``world = origin + basis * local`` with an identity basis in every
shipped object (``docs/WORLD_FIELDS.md`` §3, ``docs/WORLD_MATERIALS.md`` §6).
So the world frame is a pure translation of the file's vertex frame, and every
placement command that carries a position -- ``SET_PLAYER_START``,
``ADD_THING``, ``ADD_SPRITE``, ``ADD_ITEM``, ``ADD_LOCATION`` -- is used as is
(``VOLUME_SCALE = 1.0``): over the 36 levels 96 % of the ``ADD_SPRITE``
positions land within 5 units of their level's mesh and the ``ADD_THING``
median is 30-60 units, while the old ``1.875 * local`` frame put them 400-1500
units away.

``SCALE = 1/1.875`` is **not** a world factor.  It is only used by the
switch/door alignment checks (``tools/re/doors.py`` and the ``regress.py``
"switch positions land on the mesh" check), because the ``ADD_SWITCH``
operands 3..5 of the shipped data numerically match ``1.875 * local`` (the
``f64`` basis ``1.0`` read as its high ``f32`` dword) 85 % of the time, while
the engine itself compares them with the player position directly
(``0x44fc30``).  That is an open data question, not a transform question; see
``docs/WORLD_FIELDS.md`` §3.1.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field, replace

from . import sbt, sbtvm

#: Script frame -> ``.sbw`` *local* frame.  Kept for the switch/door checks
#: only (see the "Coordinate frame" note); positions that are world
#: coordinates must not be scaled by it.
SCALE = 1.0 / 1.875

#: Script (and engine world) -> world.  The header basis is the identity, so
#: scene positions are already world coordinates.
VOLUME_SCALE = 1.0


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

    The six source/path/site/depth/via/occ fields are filled by the opt-in
    recursive/executed APIs (:func:`things` with ``recursive=True``, :func:`executed_things`); the
    default top-level extractor leaves them at their defaults, so existing
    callers see byte-for-byte the same objects.
    """

    entity: int          # id of the entity whose sub-script builds it
    kind: str            # the ADD_THING* condition name
    type_index: int
    type_name: str = ""
    model: str = ""
    pos: tuple | None = None   # world coordinates, None when source-relative
    angles: tuple = ()
    owner: int = 0       # the L operand in front of the type (player/source)
    #: entity whose sub-script created it (same value as ``entity`` for the
    #: static extractor; the *runtime* source entity for ``via="executed"``)
    source: int = 0
    #: chain of the enclosing ``IF`` conditions, outermost first; ``()`` for a
    #: top-level ``EVAL``.  The false branch is emitted after the ``IF`` as
    #: ordinary top-level code (see docs/NESTED_THINGS.md), so ``ELSE`` only
    #: appears for a nonstandard block that carries an ELSE body.
    path: tuple = ()
    #: byte offset of the ``EVAL ADD_THING*`` instruction (unique per code)
    site: int = -1
    #: ``len(path)``: 0 = top level, >0 = inside an ``IF``/``ELSE`` body
    depth: int = 0
    #: ``"static"`` (decoded) or ``"executed"`` (VM result for a context)
    via: str = "static"
    #: per-path execution count of the site (loops can build a thing twice)
    occ: int = 0

    @property
    def absolute(self) -> bool:
        return self.pos is not None

    @property
    def nested(self) -> bool:
        """True when the thing sits inside an ``IF``/``ELSE`` body."""
        return self.depth > 0

    @property
    def top_level(self) -> bool:
        return self.depth == 0


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

    #: The nine ``D`` doubles are stored **in order** into the record
    #: (``+0x00``..``+0x40``): ``D1..D3`` is a corner, ``D4..D6`` a second
    #: point, ``D7..D9`` a size vector.  The loader then folds them with
    #: ``+0x00 += +0x30``, ``+0x08 += +0x38``, ``+0x10 += +0x40``
    #: (``0x44e368``..``0x44e3f6``, simulated from the x87 stack), i.e. the
    #: record's box is ``corner .. corner + size``.  ``0x44e0b1``..``0x44e162``
    #: and ``0x450701``..``0x450787`` interpolate that box and hand it to the
    #: volume query ``0x434887`` and to the obstacle update
    #: ``event_post(..., -22, box)`` (``0x4795b0``) queues
    #: ``SYSTEM_DOOR_CRUSH`` (crush damage); the door volume itself is updated
    #: by ``0x44f2e0``/``0x44dfc0``, independently of the event.
    #:
    #: Neither the record nor the command carries a handle, so no polygon set
    #: is addressed.  The frame of these numbers is **not** the world frame:
    #: switch positions (same command family) land within ~5 units of the level
    #: mesh, while door corners stay ~1363 units away in ``dm1`` and no constant
    #: offset or axis permutation fixes that.  Treat them as raw operands.
    #: ``params[0]``/``params[1]`` are a **sector index and an object index**
    #: into the `.sbw` level: the door toggle calls ``0x434419(params[0],
    #: params[1])``, which walks a two-level table (``ds:0x5670d8``, stride
    #: ``0x18``: ``{count, pointer}``) and returns the object.  Across all 36
    #: levels the indices are valid 1779/1779 and each door names a *different*
    #: object (1779 distinct), i.e. one leaf per door.  ``params[2]`` is a
    #: separate value (``1500``/``300``/``9999``), not part of the index.
    @property
    def sector(self) -> int:
        return self.params[0] if self.params else -1

    @property
    def object(self) -> int:
        return self.params[1] if len(self.params) > 1 else -1

    @property
    def corner(self) -> tuple:
        """First ``D`` triple -- the box corner (raw operand, not a world pos)."""
        return tuple(self.values[:3])

    @property
    def point2(self) -> tuple:
        """Second ``D`` triple (differs from the corner on the sliding doors)."""
        return tuple(self.values[3:6])

    @property
    def size(self) -> tuple:
        """``D7..D9`` -- added to the corner to make the opposite box corner."""
        return tuple(self.values[6:9])

    #: backwards-compatible aliases used by the viewer / JSON dumps
    pos = corner
    open_pos = point2

    @property
    def moving(self) -> bool:
        """Sliding door: the second point differs from the corner."""
        return (len(self.values) >= 6
                and any(abs(a - b) > 1e-6
                        for a, b in zip(self.values[:3], self.values[3:6])))

    def bbox(self) -> tuple:
        """``(min, max)`` of the door box exactly as the loader builds it."""
        lo = self.values[:3] if len(self.values) >= 3 else (0.0, 0.0, 0.0)
        sz = self.values[6:9] if len(self.values) >= 9 else (0.0, 0.0, 0.0)
        hi = tuple(lo[i] + sz[i] for i in range(3))
        return (tuple(min(lo[i], hi[i]) for i in range(3)),
                tuple(max(lo[i], hi[i]) for i in range(3)))

    def volume(self, t: float = 0.0) -> tuple:
        """The interpolated box the engine passes to the volume query."""
        lo, hi = self.bbox()
        return (tuple(lo[i] + t * (hi[i] - lo[i]) for i in range(3)),
                tuple(hi[i] - t * (hi[i] - lo[i]) for i in range(3)))

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
class Spawn:
    """``SET_PLAYER_START`` (169) / ``SET_DEATHMATCH_START`` (168).

    ``index`` is the player slot (0..7), ``pos`` the feet position and
    ``angles`` the three doubles copied into player ``+0x18/+0x20/+0x28``;
    ``angles[2]`` drives the camera yaw.  See ``docs/PLAYER.md``.
    """

    index: int
    pos: tuple
    angles: tuple = (0.0, 0.0, 0.0)
    kind: str = "campaign"          # "campaign" | "deathmatch"


EYE_HEIGHTS = {"LIGHT": 10.0, "MEDIUM": 11.0, "STEALTH": 11.0,
               "HEAVY": 12.0, "57 CHEVY": 14.0}


def _scaled_thing(t: "Thing", s) -> "Thing":
    return replace(t, pos=None if t.pos is None else s(t.pos))


def first_person(spawn, eye: float = 10.0, distance: float = 100.0):
    """Camera at *spawn* (feet + *eye*), using the proven engine basis.

    ``spawn.angles`` are ``(pitch, roll, yaw)`` (player ``+0x18/+0x20/+0x28``).
    Roll is ignored here (always 0 in the shipped starts).  The engine's basis
    ``0x41e025`` gives, with X right / Y forward / Z up::

        forward = ( sin(yaw)cos(pitch), cos(yaw)cos(pitch), -sin(pitch) )
        right   = ( cos(yaw),          -sin(yaw),           0            )
        up      = ( sin(yaw)sin(pitch), cos(yaw)sin(pitch),  cos(pitch) )

    Returns ``(eye_pos, look_at, basis_dict)`` in the scene frame.
    """
    import math
    pitch, _roll, yaw = (float(v) for v in spawn.angles)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    forward = (sy * cp, cy * cp, -sp)
    right = (cy, -sy, 0.0)
    up = (sy * sp, cy * sp, cp)
    eye_pos = (spawn.pos[0], spawn.pos[1], spawn.pos[2] + eye)
    look = tuple(eye_pos[i] + forward[i] * distance for i in range(3))
    return eye_pos, look, {"forward": forward, "right": right, "up": up,
                           "yaw": yaw, "pitch": pitch}


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
    #: player starts (``SET_PLAYER_START`` / ``SET_DEATHMATCH_START``)
    spawns: list[Spawn] = field(default_factory=list)
    #: commands seen whose operand layout is still opaque (name -> count)
    other: dict = field(default_factory=dict)
    #: opt-in: *every* static ``ADD_THING*`` including nested ``IF``/``ELSE``
    #: bodies, each tagged with ``path``/``site``/``depth`` (see
    #: :meth:`nested_things` and :func:`load` with ``execute=True``)
    recursive_things: list = field(default_factory=list)
    #: opt-in: things produced by running the VM for ``executed_for``
    executed_things: list = field(default_factory=list)
    #: the context ``executed_things`` was produced for (a ``ThingContext``)
    executed_for: object = field(default=None, repr=False, compare=False)
    #: source SCRIPT record / parsed .sbt, kept so ``nested_things(context)``
    #: can run the executor on demand
    _script: object = field(default=None, repr=False, compare=False)
    _sbt: object = field(default=None, repr=False, compare=False)

    def counts(self) -> dict:
        return {"lights": len(self.lights), "locations": len(self.locations),
                "items": len(self.items), "sounds": len(self.sounds),
                "sprites": len(self.sprites), "entities": len(self.entities),
                "things": len(self.things), "doors": len(self.doors),
                "switches": len(self.switches), "lifts": len(self.lifts),
                "spawns": len(self.spawns),
                "other": sum(self.other.values())}

    def nested_counts(self) -> dict:
        """Totals of the opt-in lists, split top-level vs nested.

        The default :meth:`counts` is deliberately unchanged; this method is
        the explicit view of the recursive list.
        """
        top = sum(1 for t in self.recursive_things if t.depth == 0)
        total = len(self.recursive_things)
        exec_top = sum(1 for t in self.executed_things if t.depth == 0)
        sites = len({t.site for t in self.recursive_things})
        return {"top_level": top, "nested": total - top, "total": total,
                "executed": len(self.executed_things),
                "executed_top_level": exec_top,
                "executed_nested": len(self.executed_things) - exec_top,
                "sites": sites, "duplicates": total - sites}

    def nested_things(self, context=None) -> list:
        """``ADD_THING*`` including nested ``IF``/``ELSE`` bodies.

        Without *context* this returns the static recursive list (every
        syntactic site, including the false-branch code that follows an
        ``IF``, tagged with ``path``/``depth``).  With a
        :class:`ThingContext` it runs the VM executor over every entity
        block in that context and returns the things the engine would actually
        build; the result is cached in ``executed_things``.

        Requires a scene produced by :func:`load`/:func:`from_sbt` (the source
        record is kept there); a hand-built :class:`Scene` raises ``ValueError``.
        """
        if context is None:
            return list(self.recursive_things)
        if self._script is None:
            raise ValueError("scene has no source script; use scene.load()")
        if self.executed_for == context and self.executed_things:
            return list(self.executed_things)
        self.executed_things = executed_things(self._script, self._sbt, context)
        self.executed_for = context
        return list(self.executed_things)

    def scaled(self, factor: float = VOLUME_SCALE) -> Scene:
        """A copy with every position multiplied by ``factor``."""

        def s(p):
            return tuple(v * factor for v in p)

        return Scene(
            lights=[Light(s(l.pos), l.rgb, l.kind, l.param, l.extra)
                    for l in self.lights],
            locations=[Location(x.thing_id, s(x.pos), x.orientation)
                       for x in self.locations],
            items=[Item(x.item_id, s(x.pos), x.fixed, x.extra) for x in self.items],
            sounds=[SoundSource(s(x.pos), x.sample, x.volume, x.pitch)
                    for x in self.sounds],
            sprites=[Sprite(s(x.pos), x.jitter, x.id, x.extra)
                     for x in self.sprites],
            entities=list(self.entities),
            things=list(self.things),
            doors=list(self.doors),
            switches=list(self.switches),
            lifts=list(self.lifts),
            spawns=[Spawn(x.index, s(x.pos), x.angles, x.kind)
                    for x in self.spawns],
            other=dict(self.other),
            recursive_things=[_scaled_thing(x, s) for x in self.recursive_things],
            executed_things=[_scaled_thing(x, s) for x in self.executed_things],
            executed_for=self.executed_for,
            _script=self._script,
            _sbt=self._sbt,
        )

    def markers(self, scale: float = VOLUME_SCALE):
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
        for d in self.doors:
            if len(d.pos) == 3:
                out.append((tuple(v * scale for v in d.pos), (120, 255, 120)))
            if d.moving:
                out.append((tuple(v * scale for v in d.open_pos), (200, 255, 200)))
        return out

    def render_lights(self, scale: float = VOLUME_SCALE):
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
_SPAWN = _op("SET_PLAYER_START")        # 169  (player slot starts)
_DM_SPAWN = _op("SET_DEATHMATCH_START")  # 168  (deathmatch respawn table)

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


#: maximum ``IF`` nesting the recursive walker will descend (the shipped
#: corpus peaks at 5; the limit turns a hostile/degenerate block into a clean
#: diagnostic instead of a Python recursion error)
MAX_THING_DEPTH = 64


class NestingError(ValueError):
    """A recursive ``ADD_THING*`` walk could not be completed safely."""


def _if_label(insn) -> str:
    """Human-readable label of an ``IF``/``ELSE`` instruction for ``path``."""
    if insn.name == "ELSE":
        return "ELSE"
    if insn.name == "IF" and insn.args:
        return "IF %s" % insn.args[0][0]
    return insn.name


def _walk_things(insns, visit, path=(), depth=0):
    """Visit ``EVAL ADD_THING*`` instructions, recursing into IF/ELSE bodies.

    ``0xFF`` ends the *sub-script*, so it stops the whole walk (not just the
    current body); this mirrors :func:`things`' top-level ``break``.  ``path``
    grows by one label per enclosing ``IF``/``ELSE``.
    """
    for i in insns:
        if i.name == "END":
            return True                     # stop the entire sub-script
        if i.op == 0xC9 and i.args and i.args[0][0].startswith("ADD_THING"):
            visit(i, path, depth)
        body = getattr(i, "body", None)
        if isinstance(body, list):
            if depth + 1 > MAX_THING_DEPTH:
                raise NestingError(
                    "ADD_THING nesting deeper than %d at 0x%x"
                    % (MAX_THING_DEPTH, i.off))
            if _walk_things(body, visit, path + (_if_label(i),), depth + 1):
                return True
    return False


def _thing_from(insn, eid, names, models, sbt_file, path=(), depth=0,
                via="static", source=None):
    """Build a :class:`Thing` from an ``EVAL ADD_THING*`` instruction."""
    cname, args = insn.args[0]
    ti, ints, dbls, owner = _thing_args(args, _cond(cname))
    if ti is None:
        return None
    name = model = ""
    if 0 <= ti < len(names):
        name = names[ti]
        if sbt_file is not None:
            model = models[_thing_model(sbt_file, ti)]
    pos = tuple(ints[:3]) if (cname in _THINGS_ABS
                              and len(ints) >= 3) else None
    return Thing(eid, cname, _s32(ti), name, model, pos, tuple(dbls), owner,
                 source=eid if source is None else source,
                 path=tuple(path), site=insn.off, depth=depth, via=via)


def things(script: sbt.Script, sbt_file=None, recursive: bool = False,
           strict: bool | None = None) -> list[Thing]:
    """Every ``ADD_THING*`` built by the level's entity sub-scripts.

    Each entity row of the table names a sub-script (its ``offset``, run by
    ``GENERATE`` through ``0x457390``); decoding that block yields the
    ``ADD_THING*`` conditions that build the entity's thing.  With *sbt_file*
    (a parsed :class:`gm.sbt.Sbt`) the type name and the ``SOB`` model are
    resolved through ``THING_TYPES``/``SOB_TABLE``.

    By default only the block's **top level** is inspected -- the historical
    behaviour (4 008 things across the 36 shipped levels).  ``recursive=True``
    is the opt-in recursive walk: it also descends into ``IF`` bodies (the
    compiler emits the false branch as top-level code after the ``IF``, so it
    is already covered), giving 7 563 static sites, and tags every result with
    ``path``, ``site``, ``depth`` and ``via="static"``.  Results are
    de-duplicated by code offset, so two entity rows naming the same block
    cannot double-count.

    ``strict`` controls a block that fails to decode or nests too deeply: with
    ``recursive=True`` it defaults to raising :class:`NestingError`, otherwise
    a bad block is skipped as before.
    """
    if strict is None:
        strict = recursive
    out: list[Thing] = []
    names: list = []
    models: list = []
    if sbt_file is not None:
        names = [t.name for t in sbt_file.records_of(4)[0].payload.entries]
        models = [e.name for e in sbt_file.records_of(20)[0].payload.entries]
    code = script.code
    seen: set = set()
    for eid, _flags, off in entities(script):
        if not 0 <= off < len(code):
            continue
        try:
            ins, _end = sbtvm.decode(code, off, len(code))
        except Exception as exc:                # noqa: BLE001 - bad block
            if strict:
                raise NestingError("cannot decode block 0x%x of entity %s: %s"
                                   % (off, eid, exc)) from exc
            continue
        if not recursive:
            for i in ins:
                if i.name == "END":
                    break
                if i.op != 0xC9 or not i.args:
                    continue
                if not i.args[0][0].startswith("ADD_THING"):
                    continue
                th = _thing_from(i, eid, names, models, sbt_file)
                if th is not None:
                    out.append(th)
            continue
        local: list[Thing] = []

        def visit(i, path, depth):
            th = _thing_from(i, eid, names, models, sbt_file, path, depth)
            if th is not None:
                local.append(th)

        try:
            _walk_things(ins, visit)
        except NestingError:
            if strict:
                raise
            continue
        for th in local:
            if th.site in seen:                 # no double counting per site
                continue
            seen.add(th.site)
            out.append(th)
    return out


#: ``ThingContext`` defaults: campaign, difficulty 1, one player, seed used by
#: the corpus sweep.  Frozen so it can key the per-scene execution cache.
@dataclass(frozen=True)
class ThingContext:
    """Runtime context for the executed ``ADD_THING*`` view.

    Every field maps to the VM state built by
    :func:`gm.sbtvm_exec.state_from_script`.  ``objectives_complete`` fills the
    level-objective bits (the condition needs the DEFINE list from the main
    block, which entity blocks do not carry).  Two contexts compare equal iff
    all fields match, so ``Scene.nested_things(context)`` can cache safely.
    """

    game_mode: int = 0          # 0 campaign, 1 dm-normal, 2 dm-classic, 5 scavenger
    difficulty: int = 1
    players: int = 1
    playertype: int | None = None
    source_player: int | None = None
    god_mode: bool = False
    objectives_complete: bool = False
    seed: int = 0x12345678
    random_unknown: bool = False
    follow_generate: bool = False
    max_steps: int = 200_000
    max_time: float = 5.0
    max_paths: int = 64
    on_unknown: str = "both"

    def label(self) -> str:
        bits = ["mode=%d" % self.game_mode, "diff=%d" % self.difficulty,
                "players=%d" % self.players]
        if self.playertype is not None:
            bits.append("type=%d" % self.playertype)
        if self.source_player is not None:
            bits.append("src_player=%d" % self.source_player)
        if self.god_mode:
            bits.append("god")
        if self.objectives_complete:
            bits.append("objectives")
        return ",".join(bits)


@dataclass
class ExecutedThings:
    """Result of one :func:`execute_things` run."""

    things: list = field(default_factory=list)
    context: ThingContext | None = None
    reasons: dict = field(default_factory=dict)
    paths: int = 0
    blocks: int = 0
    spawns: list = field(default_factory=list)

    def counts(self) -> dict:
        top = sum(1 for t in self.things if t.depth == 0)
        return {"top_level": top, "nested": len(self.things) - top,
                "total": len(self.things), "spawns": len(self.spawns)}


def _block_offsets(script):
    """Entity-table offsets, in table order, de-duplicated."""
    out = []
    seen = set()
    for _eid, _flags, off in entities(script):
        if off not in seen and 0 <= off < len(script.code):
            seen.add(off)
            out.append((_eid, off))
    return out


def _static_paths(script, sbt_file):
    """``site -> (path, depth)`` from the recursive static walk."""
    return {t.site: (t.path, t.depth)
            for t in things(script, sbt_file, recursive=True)}


def _objectives_from(script):
    """The ``DEFINE_LEVEL_OBJECTIVES`` list of a script (best effort)."""
    try:
        ins, _end = sbtvm.decode(script.code)
    except Exception:                       # noqa: BLE001
        return []
    for i in walk_all(ins):
        if i.name == "DEFINE_LEVEL_OBJECTIVES" and isinstance(i.args, list):
            return [s for s in i.args if isinstance(s, str)]
    return []


def walk_all(insns):
    """Every instruction of a block, recursing into IF/ELSE bodies."""
    for i in insns:
        yield i
        if isinstance(getattr(i, "body", None), list):
            yield from walk_all(i.body)


def execute_things(script: sbt.Script, sbt_file=None, context=None,
                   executor=None, path_map=None) -> ExecutedThings:
    """Run the VM over every entity block and collect the things it builds.

    This is the runtime counterpart of :func:`things`: each entity sub-script
    is started with ``source_entity`` set to its id, every ``IF`` the model
    cannot decide is forked (``on_unknown="both"``) and the per-path scenes
    are merged by ``(site, occurrence)`` -- the exact merge used by
    ``tools/re/vm_exec.py --corpus``, so a loop that builds a thing twice keeps
    both, while the same site in two contexts is counted once.

    The returned things carry ``via="executed"``, the runtime ``source`` entity
    and the *syntactic* ``path``/``depth`` of their site (context tells which
    branch ran, the path tells where the site sits).  Spawns are collected in
    ``ExecutedThings.spawns`` for the 391/391 check.
    """
    from . import sbtvm_exec as vm

    ctx = context or ThingContext()
    names: list = []
    models: list = []
    if sbt_file is not None:
        names = [t.name for t in sbt_file.records_of(4)[0].payload.entries]
        models = [e.name for e in sbt_file.records_of(20)[0].payload.entries]
    paths = _static_paths(script, sbt_file) if path_map is None else path_map

    st = vm.state_from_script(
        script, game_mode=ctx.game_mode, player_count=ctx.players,
        difficulty=ctx.difficulty, players=ctx.players, seed=ctx.seed,
        playertype=ctx.playertype, source_player=ctx.source_player,
        god_mode=ctx.god_mode)
    if ctx.objectives_complete:
        objs = _objectives_from(script)
        st.objectives = objs
        st.objectives_bits = (1 << len(objs)) - 1 if objs else 0
    ex = executor or vm.Executor(
        st, max_steps=ctx.max_steps, max_time=ctx.max_time,
        max_paths=ctx.max_paths, on_unknown=ctx.on_unknown,
        follow_generate=ctx.follow_generate,
        random_unknown=ctx.random_unknown)
    result = ExecutedThings(context=ctx)
    seen = set()
    for eid, off in _block_offsets(script):
        rs = ex.run(off, source_entity=eid)
        result.blocks += 1
        result.paths += len(rs)
        for r in rs:
            result.reasons[r.reason] = result.reasons.get(r.reason, 0) + 1
        merged = vm.merge_scenes(rs)
        for obj in merged.spawns:
            key = ("spawns", getattr(obj, "site", None),
                   getattr(obj, "occ", None))
            if key in seen:
                continue
            seen.add(key)
            if len(result.spawns) < 100_000:
                result.spawns.append(obj)
        for t in merged.things:
            key = ("things", t.site, getattr(t, "occ", 0))
            if key in seen:
                continue
            path, depth = paths.get(t.site, ((), 0))
            th = Thing(t.entity if t.entity is not None else eid, t.kind,
                       t.type_index, "", "", t.pos, tuple(t.angles), t.owner,
                       source=t.entity if t.entity is not None else eid,
                       path=path, site=t.site, depth=depth, via="executed",
                       occ=getattr(t, "occ", 0))
            if 0 <= th.type_index < len(names):
                th.type_name = names[th.type_index]
                if sbt_file is not None:
                    th.model = models[_thing_model(sbt_file, th.type_index)]
            result.things.append(th)
    return result


def executed_things(script: sbt.Script, sbt_file=None, context=None) -> list[Thing]:
    """The :func:`execute_things` list, without the run statistics."""
    return execute_things(script, sbt_file, context).things


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
        a = i.args
        if op in (_SPAWN, _DM_SPAWN) and len(a) >= 7:
            # LLLLAAA: u32 slot, s32 x/y/z, then three f64 angles.
            scene.spawns.append(Spawn(
                _s32(a[0]),
                tuple(_s32(a[k]) for k in (1, 2, 3)),
                tuple(float(a[k]) for k in (4, 5, 6)),
                "campaign" if op == _SPAWN else "deathmatch"))
            continue
        if op not in (_SKY, _LOCAL, _LOCATION, _ITEM, _ITEM_FIXED, _SOUND,
                      _SPRITE, _DOOR, _SWITCH, _LIFT):
            if isinstance(i.name, str) and i.name in _SEEN:
                scene.other[i.name] = scene.other.get(i.name, 0) + 1
            continue
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


def from_sbt(path_or_file, execute: bool = False, context=None) -> Scene:
    """``Scene`` of a parsed :class:`gm.sbt.Sbt` or of an ``.sbt`` path.

    The default result is exactly the historical scene.  With ``execute=True``
    the opt-in ``Scene.recursive_things`` list is filled from the recursive
    static walk (top-level *and* nested ``IF``/``ELSE`` sites); with a
    :class:`ThingContext` the VM is run for that context and
    ``Scene.executed_things`` is filled too.
    """
    f = path_or_file
    if isinstance(f, str):
        f = sbt.parse_file(f)
    recs = f.records_of(11)
    if not recs:
        raise ValueError("no SCRIPT record")
    script = recs[0].payload
    scene = extract(script)
    scene.things = things(script, f)
    scene._script = script
    scene._sbt = f
    if execute or context is not None:
        scene.recursive_things = things(script, f, recursive=True)
    if context is not None:
        scene.nested_things(context)
    return scene


def load(path: str, execute: bool = False, context=None) -> Scene:
    """Scene of an ``.sbt`` file, in the engine's world coordinates.

    ``execute=False`` (default) is unchanged.  ``execute=True`` exposes the
    nested things through ``scene.recursive_things`` /
    ``scene.nested_things()``; a :class:`ThingContext` additionally runs the
    VM (:func:`execute_things`).
    """
    return from_sbt(sbt.parse_file(path), execute=execute, context=context)
