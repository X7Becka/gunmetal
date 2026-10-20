"""Gunmetal ".soe" thing-type definitions (text source of .sbt THING_TYPES).

The engine loads ``thing\\<name>.soe`` as text (loader ``gundx.exe`` VMA
``0x47cb6c``, ``fopen(.., "rt")``) and compiles it into the ``THING_TYPES``
binary record of a saved ``.sbt`` (``char name[16]`` + 49 ``double`` + 528
bytes of integers).  Only one file ships (``SOBS/railrock.soe``), so the grammar
below was recovered from the loader itself, not from samples.

The file is a plain whitespace-separated token stream read with ``fscanf``.
The first token is a **version** (0..13); later blocks are gated on it.  The
loader reads the fields in this exact order (``%ld`` integer, ``%lf`` double,
``%s`` string):

    version
    %ld %ld %s %ld %ld                       id / class-name block
    [v>=9]  %ld
    %s %lf*6 %s                              collision type, 6 doubles, shape
    [shape == ellipsoid] %lf %lf %lf         radii
    [shape == sphere]    %lf                 radius
    %lf %lf %lf %lf                          0x50/0x58/0x60/0x68
    [v>=7]  %s %ld   |   %s %s %ld           0x1d4
    [v>=8]  %ld*4    |   %ld                 0x1d8..0x1e4 (max shield values)
    %s %s                                    texture info
    %ld                                      expire value (0x1e8)
    [v>=3]  %s                               expire event
    %s %lf %lf %lf %ld %s %s %ld %ld %ld     "0x2f2" block (10 fields)
    [v>=4]  %lf                              falling fear (0xc8)
    [v>=5]  %s                               create event
    [v>=6]  %s                               morale failure event
    [v>=6]  %s                               time out event
    [v>=6]  %s                               damaged event
    [v>=7]  %ld %ld                          0x1cc / 0x1d0
    [v>=10] %lf %lf                          0xa8 / 0xb0
    [v>=11] %lf %lf                          0xb8 / 0xc0
    [v>=12] %lf %lf                          0x18 / 0x70
    [v>=13] %s %lf                           leash location name / 0xd0
    [v>=2]  %ld                              path-point count, then count *%s
    %ld                                      weapon count, then count *(%s %ld*6)
    [v>=1]  %ld                              children count, then count *
                                             (%s %lf*6 %ld)

Field labels use the engine offsets (``fXX`` = byte 0xXX of the 936-byte
``THING_TYPES`` record, ``0x000..0x3a7``) where the role is unknown; the role is
named where the loader's own error strings reveal it (shields, events, weapons,
children, path points, ...).  ``parse`` verifies that the whole token stream is
consumed (``ThingType.extra == 0`` for a well-formed file).

Two legacy labels are misleading and kept only for the old CLI/API:

* ``collision_type`` is the **movement type** token (``none``, ``plain``,
  ``turret``, ``flee``, ``attack``, ``pushable``, ...), resolved by ``0x46c840``
  against the 19-entry name table at ``0x4ba8c8`` and stored at record
  ``+0x1b4``.  The token after the six doubles is the actual collision *shape*
  (``none``/``sphere``/``ellipsoid``) stored at ``+0x1a0``.
* ``str_c8`` / ``str_f0`` / ``str_1a4`` are only local scratch labels: the three
  strings of the 10-field block resolve to a *destroy event* (``+0x1f8``), a
  *death event* (``+0x1c4``) and a *sound sample* (``+0x1fc``).

``field_map()`` returns the full offset -> role table with evidence/status, and
``describe()`` renders a parsed file against it.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

__all__ = ["Field", "FieldInfo", "ThingType", "SoeError", "parse", "parse_file",
           "parse_text", "field_map", "describe", "label_info", "FIELD_MAP",
           "RECORD_SIZE"]

MAX_VERSION = 13

#: Size of one ``THING_TYPES`` record (``char name[16]`` + 49 doubles + 528 bytes
#: of integers).  The loader ``0x47cb6c`` walks the runtime array at ``0x776d60``
#: with this stride.
RECORD_SIZE = 0x3A8


@dataclass(frozen=True)
class FieldInfo:
    """One proven (or hypothesised) field of the 936-byte thing-type record.

    ``offset`` is relative to the record base ``0x776d60``.  ``size`` is 4 for
    an ``int``, 8 for a ``double``; array fields use ``size`` of one element and
    set ``count``/``stride``.  ``soe`` is the parser label (or ``prefix[]``
    pattern) that feeds the field; computed fields have no ``.soe`` token.
    """

    offset: int
    size: int
    kind: str                 # "int" | "double" | "name16"
    name: str
    role: str
    status: str               # "proven" | "hypothesis" | "unknown"
    loader: str = ""          # loader instruction / format string
    evidence: tuple[str, ...] = ()   # xrefs of consumers / proof sites
    soe: str | None = None
    count: int = 1
    stride: int = 0
    legacy: str | None = None
    instance: int | None = None      # runtime instance offset (base 0x7ac0c8)

    @property
    def end(self) -> int:
        if self.count <= 1:
            return self.offset + self.size
        step = self.stride or self.size
        return self.offset + self.size + step * (self.count - 1)


#: The map below is the result of ``tools/re/soe_semantics.py``.  Status
#: semantics:
#:
#: * ``proven``     - the loader's format/error string or an unambiguous
#:   consumer fixes both the offset and the role;
#: * ``hypothesis`` - the offset is fixed, the role is inferred from one or two
#:   consumers but not named anywhere in the executable;
#: * ``unknown``    - only the offset/width is known.
FIELD_MAP: tuple[FieldInfo, ...] = (
    FieldInfo(0x000, 16, "name16", "name", "thing-type name (search key)",
              "proven", "copy at 0x47cb6c; lookup 0x47cb00",
              ("0x47cb7a", "0x47cb9f", "0x47cb23")),
    FieldInfo(0x010, 8, "double", "off_z", "spawn height offset (hypothesis)",
              "hypothesis", "%lf 0x47cff8", ("0x46d0ff", "0x46d10b"),
              soe="col_f10", instance=0x58),
    FieldInfo(0x018, 8, "double", "f18", "unknown (v>=12)",
              "unknown", "%lf 0x47d98f", ("0x46dd17",), soe="f18", instance=0x60),
    FieldInfo(0x020, 8, "double", "f20", "unknown (physics/scale?)",
              "hypothesis", "%lf 0x47cff8", ("0x471552", "0x472c42"),
              soe="col_f20", instance=0x18),
    FieldInfo(0x028, 8, "double", "f28", "unknown (physics/scale?)",
              "hypothesis", "%lf 0x47cff8", ("0x471604", "0x47495a"),
              soe="col_f28", instance=0x20),
    FieldInfo(0x030, 8, "double", "radius", "bounding radius (computed)",
              "proven", "computed at 0x47cf61 / 0x47cfad",
              ("0x47cec7", "0x47cf23"), soe="radius/radius_max", instance=0x30),
    FieldInfo(0x038, 8, "double", "radius_x", "ellipsoid radius X",
              "proven", "0x47ce91 %lf %lf %lf", ("0x47ced3",),
              soe="radius_x", instance=0x38),
    FieldInfo(0x040, 8, "double", "radius_y", "ellipsoid radius Y",
              "proven", "0x47ce91", ("0x47cefe",), soe="radius_y", instance=0x40),
    FieldInfo(0x048, 8, "double", "radius_z", "ellipsoid radius Z",
              "proven", "0x47ce91", ("0x47cf04",), soe="radius_z", instance=0x48),
    FieldInfo(0x050, 8, "double", "f50", "unknown double",
              "unknown", "%lf 0x47cfc9", ("0x46dce7",), soe="f50", instance=0x50),
    FieldInfo(0x058, 8, "double", "f58", "unknown double",
              "unknown", "%lf 0x47cfc9", ("0x46dbeb",), soe="f58", instance=0x68),
    FieldInfo(0x060, 8, "double", "f60", "unknown double (not copied to the instance)",
              "unknown", "%lf 0x47cfc9; read with +0x50/+0x58/+0x68 at 0x47cfcf",
              ("0x47cfb3",), soe="f60"),
    FieldInfo(0x068, 8, "double", "f68", "unknown double (copied to instance +0x70)",
              "unknown", "%lf 0x47cfc9", ("0x46da94", "0x46daa6"), soe="f68",
              instance=0x70),
    FieldInfo(0x070, 8, "double", "f70", "unknown (v>=12)",
              "unknown", "%lf 0x47d98f", ("0x46dbd9",), soe="f70", instance=0x78),
    FieldInfo(0x078, 8, "double", "f78", "unknown double",
              "unknown", "%lf 0x47d347", ("0x46da64",), soe="f78", instance=0x90),
    FieldInfo(0x080, 8, "double", "f80", "unknown double",
              "unknown", "%lf 0x47d347", ("0x46deff",), soe="f80", instance=0x80),
    FieldInfo(0x088, 8, "double", "f88", "unknown double",
              "unknown", "%lf 0x47d347", ("0x46df0b",), soe="f88", instance=0x88),
    FieldInfo(0x090, 8, "double", "col_f90", "collision grid/step value",
              "hypothesis", "%lf 0x47ccf9; *0.000191794 at 0x456f40",
              ("0x456f40", "0x4a2550"), soe="col_f90", instance=0x00),
    FieldInfo(0x098, 8, "double", "col_f98", "collision grid/step value",
              "hypothesis", "%lf 0x47ccf9; *0.000191794",
              ("0x456f40",), soe="col_f98", instance=0x08),
    FieldInfo(0x0A0, 8, "double", "col_fa0", "collision grid/step value",
              "hypothesis", "%lf 0x47ccf9; *0.000191794",
              ("0x456f40",), soe="col_fa0", instance=0x10),
    FieldInfo(0x0A8, 8, "double", "f_a8", "unknown (v>=10)",
              "unknown", "%lf 0x47d87f", ("0x46d9fe",), soe="f_a8", instance=0x1e0),
    FieldInfo(0x0B0, 8, "double", "f_b0", "unknown (v>=10)",
              "unknown", "%lf 0x47d87f", ("0x46da16",), soe="f_b0", instance=0x1e8),
    FieldInfo(0x0B8, 8, "double", "f_b8", "unknown (v>=11)",
              "unknown", "%lf 0x47d87f", ("0x46da2e",), soe="f_b8", instance=0x1f0),
    FieldInfo(0x0C0, 8, "double", "f_c0", "unknown (v>=11)",
              "unknown", "%lf 0x47d87f", ("0x46da46",), soe="f_c0", instance=0x1f8),
    FieldInfo(0x0C8, 8, "double", "falling_fear", "falling fear",
              "proven", "%lf 0x47d565; error 0x4bb838",
              ("0x47d580",), soe="falling_fear", instance=0x188),
    FieldInfo(0x0D0, 8, "double", "leash_distance", "leash distance/radius",
              "hypothesis", "v>=13, follows leash location 0x47d9f1",
              ("0x46dad0",), soe="f_d0", instance=0x248),
    FieldInfo(0x0D8, 8, "double", "child_x", "child offset X (4 entries, stride 0x18)",
              "proven", "children block 0x47de02; stride 0x18 at 0x46de48",
              ("0x46d229",), soe="child[i].c0", count=4, stride=0x18),
    FieldInfo(0x0E0, 8, "double", "child_y", "child offset Y (4 entries, stride 0x18)",
              "proven", "children block 0x47de02", ("0x46d273",),
              soe="child[i].c1", count=4, stride=0x18),
    FieldInfo(0x0E8, 8, "double", "child_z", "child offset Z (4 entries, stride 0x18)",
              "proven", "children block 0x47de02", ("0x46d264",),
              soe="child[i].c2", count=4, stride=0x18),
    FieldInfo(0x138, 8, "double", "child_r1", "child angle/scale 1 (4 entries, stride 8)",
              "proven", "children block 0x47de02", ("0x46d259",),
              soe="child[i].c3", count=4, stride=8),
    FieldInfo(0x158, 8, "double", "child_r2", "child angle/scale 2 (4 entries, stride 8)",
              "proven", "children block 0x47de02", ("0x46d23a",),
              soe="child[i].c4", count=4, stride=8),
    FieldInfo(0x178, 8, "double", "child_r3", "child angle/scale 3 (4 entries, stride 8)",
              "proven", "children block 0x47de02", ("0x46d229",),
              soe="child[i].c5", count=4, stride=8),
    FieldInfo(0x198, 4, "int", "f198", "spawn guard upper bound (value 10)",
              "hypothesis", "%ld 0x47cc6f; check 0x46d95c",
              ("0x46d95c", "0x83d62c"), soe="f198"),
    FieldInfo(0x19C, 4, "int", "f19c", "spawn guard lower bound (value 0)",
              "hypothesis", "%ld 0x47cc6f; check 0x46d950",
              ("0x46d950", "0x83d62c"), soe="f19c"),
    FieldInfo(0x1A0, 4, "int", "collision_kind", "0 none / 1 sphere / 2 ellipsoid",
              "proven", "0x47ce51..0x47cfad; switch 0x46e282",
              ("0x47cf23", "0x47cfad", "0x46e282"), soe="collision_shape",
              instance=0x2E8),
    FieldInfo(0x1A4, 4, "int", "sob_index", "index into SOB_TABLE (model)",
              "proven", "0x47cd57 call 0x47c9b0; store 0x47cd75",
              ("0x46d0a0", "0x46d308", "engine/src/res/scene.cpp:resolve_thing_tables"),
              soe="class_name", instance=0x2F4),
    FieldInfo(0x1A8, 4, "int", "flags_a", "thing flag word A",
              "hypothesis", "%ld 0x47cc6f; bits tested 0x1/0x2/0x20/0x20000",
              ("0x46d992", "0x46c96e", "0x473aef"), soe="f1a8", instance=0x318),
    FieldInfo(0x1AC, 4, "int", "flags_b", "thing flag word B",
              "hypothesis", "%ld 0x47cc6f; bits tested 0x40000/0x1000000/0x40000000",
              ("0x46d99e", "0x471593", "0x473967"), soe="f1ac", instance=0x31C),
    FieldInfo(0x1B0, 4, "int", "flags_c", "thing flag word C (v>=9)",
              "hypothesis", "%ld 0x47ccb9; bits tested 0x40000/0x1000000",
              ("0x46d9aa", "0x46d137", "0x471a7c"), soe="f1b0", instance=0x320),
    FieldInfo(0x1B4, 4, "int", "movement_type",
              "movement type 0..18 (0 none, 13 flee, 14 attack, 18 pushable)",
              "proven", "0x46c840 table 0x4ba8c8; store 0x47d4e6",
              ("0x46c840", "0x4ba8c8", "0x47093c", "0x472610"),
              soe="collision_type", legacy="collision_type", instance=0x348),
    FieldInfo(0x1B8, 4, "int", "event_low_health",
              "event when health crosses 50%",
              "hypothesis", "%ld 0x47d03a; trigger 0x4704a9",
              ("0x4704a9", "0x470482"), soe="event_a", instance=0x34C),
    FieldInfo(0x1BC, 4, "int", "event_morale_failure", "morale-failure event",
              "proven", "error 0x4bb7a0; store 0x47d690",
              ("0x47d6b0", "0x470b02"), soe="morale_failure_event", instance=0x4CC),
    FieldInfo(0x1C0, 4, "int", "event_timeout", "time-out event",
              "proven", "error 0x4bb738; store 0x47d6f2",
              ("0x47d6f2", "0x470d4b"), soe="time_out_event", instance=0x334),
    FieldInfo(0x1C4, 4, "int", "event_death",
              "event on hit-count death (second 10-block string)",
              "hypothesis", "resolve 0x47d4a9; trigger 0x47573a",
              ("0x47573a", "0x475646"), soe="str_f0", instance=0x2E0),
    FieldInfo(0x1C8, 4, "int", "hits",
              "hit count / toughness (decremented per impact)",
              "hypothesis", "%ld 0x47d347; 0x475615",
              ("0x475615", "0x475624"), soe="f1c8", instance=0x2E4),
    FieldInfo(0x1CC, 4, "int", "attack_range",
              "attack/fire range (distance threshold)",
              "hypothesis", "v>=7 0x47d7c0; 0x473fb4",
              ("0x473fb4", "0x47615a"), soe="f1cc", instance=0x2EC),
    FieldInfo(0x1D0, 4, "int", "burst_period",
              "AI/weapon period (divisor, 0..10)",
              "hypothesis", "v>=7 0x47d7c0; 0x473eed / 0x476523",
              ("0x473eed", "0x476523"), soe="f1d0", instance=0x2F0),
    FieldInfo(0x1D4, 4, "int", "health", "hit points (max = current at spawn)",
              "hypothesis", "%ld 0x47d03a; copy 0x46dc1b/0x46dc21; damage 0x470469",
              ("0x46dc1b", "0x46dc21", "0x470469"), soe="f1d4", instance=0x350),
    FieldInfo(0x1D8, 4, "int", "shield0", "max shield layer 0",
              "proven", "0x4b9640 / error 0x4bb974", ("0x470452", "0x47045d"),
              soe="shield0", instance=0x368),
    FieldInfo(0x1DC, 4, "int", "shield1", "max shield layer 1",
              "proven", "0x4b9640", ("0x45e7cb",), soe="shield1", instance=0x36C),
    FieldInfo(0x1E0, 4, "int", "shield2", "max shield layer 2",
              "proven", "0x4b9640", ("0x45e7e5",), soe="shield2", instance=0x370),
    FieldInfo(0x1E4, 4, "int", "shield3", "max shield layer 3",
              "proven", "0x4b9640", ("0x45e7ff",), soe="shield3", instance=0x374),
    FieldInfo(0x1E8, 4, "int", "expire_ticks",
              "lifetime ticks (loader stores 0.91 * token)",
              "proven", "%ld 0x47d24b; scale 0x47d3fb; tick 0x470c9c",
              ("0x470c9c", "0x470cad", "0x4703a0"), soe="expire_value", instance=0x328),
    FieldInfo(0x1EC, 4, "int", "event_expire", "expire event",
              "proven", "error 0x4bb8b0; store 0x47d2bd; trigger 0x4703f3",
              ("0x4703f3", "0x47d2b8"), soe="expire_event", instance=0x32C),
    FieldInfo(0x1F0, 4, "int", "event_create", "create event",
              "proven", "error 0x4bb808; store 0x47d5e4; trigger 0x46e275",
              ("0x46e275", "0x46f6df"), soe="create_event", instance=0x38C),
    FieldInfo(0x1F4, 4, "int", "event_damaged", "damaged event",
              "proven", "error 0x4bb6d4; store 0x47d755; trigger 0x4709a2",
              ("0x470995", "0x4709a2"), soe="damaged_event", instance=0x4D0),
    FieldInfo(0x1F8, 4, "int", "event_destroy",
              "event when thing is removed/destroyed (first 10-block string)",
              "hypothesis", "resolve 0x47d450; trigger 0x4702e9",
              ("0x4702e9", "0x470338"), soe="str_c8", instance=0x390),
    FieldInfo(0x1FC, 4, "int", "sound_id", "sound sample index (loop sound)",
              "proven", "0x40b9c0 table 0x4c9c90; store 0x47d506; play 0x4720bf",
              ("0x40b9c0", "0x472080"), soe="str_1a4", instance=0x388),
    FieldInfo(0x200, 4, "int", "sound_volume",
              "sound volume (loader stores 0.64 * token)",
              "hypothesis", "scale 0x47d50c; play 0x4720bf",
              ("0x4720bf", "0x46b960"), soe="f200", instance=0x384),
    FieldInfo(0x204, 4, "int", "leash_location", "leash location identifier",
              "proven", "error 0x4bb658; resolve 0x47da4f",
              ("0x47da4f", "0x46db49"), soe="leash_location", instance=None),
    FieldInfo(0x208, 4, "int", "texture_a", "texture id A",
              "proven", "0x42b81b; store 0x47d1de",
              ("0x42b81b", "0x47d1de"), soe="tex_a", instance=0x378),
    FieldInfo(0x20C, 4, "int", "texture_b", "texture id B",
              "proven", "0x42b81b; store 0x47d201",
              ("0x42b81b", "0x47d201"), soe="tex_b", instance=0x37C),
    FieldInfo(0x210, 4, "int", "timer_period",
              "periodic AI/weapon period (divisor)",
              "hypothesis", "%ld 0x47d347; 0x473eed / 0x476523",
              ("0x473eed", "0x476523"), soe="f210", instance=0x3A4),
    FieldInfo(0x214, 4, "int", "timer_step",
              "periodic AI/weapon increment (loader stores 0.91 * token)",
              "hypothesis", "scale 0x47d52f; 0x473f07 / 0x47653d",
              ("0x473f07", "0x47653d"), soe="f214", instance=0x3A8),
    FieldInfo(0x218, 4, "int", "weapon_count", "number of weapons (<=8)",
              "proven", "%ld 0x47dbbb; clamp 0x47dc2c",
              ("0x47dc2c", "0x46dd7d"), soe="weapon_count", instance=0x3AC),
    FieldInfo(0x21C, 4, "int", "weapon_id", "weapon identifier (resolved)",
              "proven", "0x45fb90 in weapon loop 0x47dd21",
              ("0x47dd21", "0x46ddc6"), soe="weapon[i].name", count=8, stride=4),
    FieldInfo(0x23C, 4, "int", "weapon_p0", "weapon parameter 0 (range?)",
              "hypothesis", "%ld 0x47dcaa; 0x473def/0x473e40",
              ("0x473def", "0x473e40"), soe="weapon[i].w0", count=8, stride=4),
    FieldInfo(0x25C, 4, "int", "weapon_p1", "weapon parameter 1 (range?)",
              "hypothesis", "%ld 0x47dcaa; 0x473dfc/0x473e4d",
              ("0x473dfc", "0x473e4d"), soe="weapon[i].w1", count=8, stride=4),
    FieldInfo(0x27C, 4, "int", "weapon_p2",
              "weapon parameter 2 (cooldown; loader stores 0.91 * token)",
              "hypothesis", "%ld 0x47dcaa; scale 0x47dcff; cooldown 0x473ecc",
              ("0x473ec3", "0x47134f"), soe="weapon[i].w2", count=8, stride=4),
    FieldInfo(0x29C, 4, "int", "weapon_p3", "weapon parameter 3 (double at runtime)",
              "unknown", "%ld 0x47dcaa; 0x46ddb2",
              ("0x46ddb2",), soe="weapon[i].w3", count=8, stride=4),
    FieldInfo(0x2BC, 4, "int", "weapon_p4", "weapon parameter 4 (double at runtime)",
              "unknown", "%ld 0x47dcaa; 0x46ddbb",
              ("0x46ddbb",), soe="weapon[i].w4", count=8, stride=4),
    FieldInfo(0x2DC, 4, "int", "weapon_p5", "weapon parameter 5 (double at runtime)",
              "unknown", "%ld 0x47dcaa; 0x46ddc0",
              ("0x46ddc0",), soe="weapon[i].w5", count=8, stride=4),
    FieldInfo(0x2FC, 4, "int", "children_count", "number of children (<=4)",
              "proven", "%ld 0x47dd7f; clamp 0x47ddad",
              ("0x47ddad", "0x46d192"), soe="children_count", instance=None),
    FieldInfo(0x300, 4, "int", "child_type", "child thing-type index",
              "proven", "0x47cb00 call 0x47de1a; store 0x47de28",
              ("0x47de1a", "0x46d293"), soe="child[i].name", count=4, stride=4),
    FieldInfo(0x310, 4, "int", "child_param", "child extra integer",
              "proven", "%ld 0x47de02", ("0x46d3ec",), soe="child[i].x",
              count=4, stride=4),
    FieldInfo(0x320, 4, "int", "path_count", "path-point count (<=32)",
              "proven", "%ld 0x47dab5; clamp 0x47daea",
              ("0x47daea", "0x46de66"), soe="path_point_count", instance=0x438),
    FieldInfo(0x324, 4, "int", "path_point", "path location identifier",
              "proven", "0x460000 in loop 0x47db3d",
              ("0x47db3d", "0x46de90"), soe="path_point[i]", count=32, stride=4,
              instance=0x43C),
)

#: Maximum counts enforced by the loader (``0x47dc2c``, ``0x47ddad``,
#: ``0x47daea``).
MAX_WEAPONS = 8
MAX_CHILDREN = 4
MAX_PATH_POINTS = 32


def field_map() -> list[FieldInfo]:
    """Return the offset -> role map of the 936-byte ``THING_TYPES`` record."""
    return list(FIELD_MAP)



class SoeError(ValueError):
    """Raised when a .soe stream ends earlier than the grammar requires."""


@dataclass
class Field:
    label: str
    kind: str                       # "int" | "flt" | "str"
    value: int | float | str
    offset: int | None = None       # engine byte offset, when known


@dataclass
class ThingType:
    name: str
    version: int
    fields: list[Field] = field(default_factory=list)
    ctx: dict[str, object] = field(default_factory=dict)   # name -> last value
    extra: int = 0                  # leftover tokens (0 = fully consumed)
    lines: list[str] = field(default_factory=list)  # exact text lines (round-trip)

    def get(self, label: str, default=None):
        return self.ctx.get(label, default)

    def to_bytes(self) -> bytes:
        """Re-serialise with CRLF endings (byte-exact round-trip of shipped files)."""
        lines = self.lines or self.render_lines()
        return b"\r\n".join(ln.encode("latin1") for ln in lines)

    def render_lines(self) -> list[str]:
        """Canonical one-value-per-line text built from the parsed fields."""
        return [str(f.value) for f in self.fields]


class _Reader:
    __slots__ = ("tok", "i", "n")

    def __init__(self, text: str):
        self.tok = text.split()
        self.i = 0
        self.n = len(self.tok)

    def eof(self) -> bool:
        return self.i >= self.n

    def s(self) -> str:
        if self.i >= self.n:
            raise SoeError("unexpected end of .soe")
        v = self.tok[self.i]
        self.i += 1
        return v

    def d(self) -> int:
        if self.i >= self.n:
            raise SoeError("unexpected end of .soe (expected an integer)")
        v = self.tok[self.i]
        self.i += 1
        try:
            return int(v, 10)
        except ValueError:
            raise SoeError(f"expected an integer, got {v!r}") from None

    def f(self) -> float:
        if self.i >= self.n:
            raise SoeError("unexpected end of .soe (expected a number)")
        v = self.tok[self.i]
        self.i += 1
        try:
            return float(v)
        except ValueError:
            raise SoeError(f"expected a number, got {v!r}") from None


def parse_text(text: str, name: str = "") -> ThingType:
    r = _Reader(text)
    tt = ThingType(name=name, version=0)

    def emit(label: str, kind: str, value, offset=None):
        tt.fields.append(Field(label, kind, value, offset))
        tt.ctx[label] = value
        return value

    def gi(label, off=None):
        return emit(label, "int", r.d(), off)

    def gf(label, off=None):
        return emit(label, "flt", r.f(), off)

    def gs(label, off=None):
        return emit(label, "str", r.s(), off)

    v = gi("version")
    tt.version = v

    # id / class-name block
    gi("f19c", 0x19C)
    gi("f198", 0x198)
    gs("class_name")
    tt.ctx["model_name"] = tt.ctx["class_name"]   # SOB_TABLE key, not a class
    gi("f1a8", 0x1A8)
    gi("f1ac", 0x1AC)

    if v >= 9:
        gi("f1b0", 0x1B0)

    # collision
    emit("collision_type", "str", r.s())
    tt.ctx["movement_type"] = tt.ctx["collision_type"]
    gf("col_f10", 0x10)
    gf("col_f20", 0x20)
    gf("col_f28", 0x28)
    gf("col_f90", 0x90)
    gf("col_f98", 0x98)
    gf("col_fa0", 0xA0)
    shape = r.s().lower()
    emit("collision_shape", "str", shape)
    if shape == "ellipsoid":
        gf("radius_x"); gf("radius_y"); gf("radius_z")
    elif shape == "sphere":
        gf("radius")

    gf("f50", 0x50)
    gf("f58", 0x58)
    gf("f60", 0x60)
    gf("f68", 0x68)

    if v >= 7:
        gs("event_a")
        gi("f1d4", 0x1D4)
    else:
        gs("event_a")
        gs("event_b")
        gi("f1d4", 0x1D4)

    if v >= 8:
        for i in range(4):
            gi(f"shield{i}")
    else:
        gi("shield0")

    gs("tex_a")
    gs("tex_b")

    gi("expire_value", 0x1E8)
    if v >= 3:
        gs("expire_event")

    gs("str_c8")
    gf("f78", 0x78)
    gf("f80", 0x80)
    gf("f88", 0x88)
    gi("f1c8", 0x1C8)
    gs("str_f0")
    gs("str_1a4")
    tt.ctx["sound_name"] = tt.ctx["str_1a4"]
    gi("f200", 0x200)
    gi("f210", 0x210)
    gi("f214", 0x214)

    if v >= 4:
        gf("falling_fear", 0xC8)
    if v >= 5:
        gs("create_event")
    if v >= 6:
        gs("morale_failure_event")
        gs("time_out_event")
        gs("damaged_event")

    if v >= 7:
        gi("f1cc", 0x1CC)
        gi("f1d0", 0x1D0)
    if v >= 10:
        gf("f_a8", 0xA8); gf("f_b0", 0xB0)
    if v >= 11:
        gf("f_b8", 0xB8); gf("f_c0", 0xC0)
    if v >= 12:
        gf("f18", 0x18); gf("f70", 0x70)
    if v >= 13:
        gs("leash_location")
        gf("f_d0", 0xD0)

    if v >= 2:
        n = gi("path_point_count")
        for i in range(max(0, n)):
            gs(f"path_point[{i}]")

    n = gi("weapon_count")
    for i in range(max(0, n)):
        gs(f"weapon[{i}].name")
        for k in range(6):
            gi(f"weapon[{i}].w{k}")

    if v >= 1:
        n = gi("children_count")
        for i in range(max(0, n)):
            gs(f"child[{i}].name")
            for k in range(6):
                gf(f"child[{i}].c{k}")
            gi(f"child[{i}].x")

    tt.extra = r.n - r.i
    tt.lines = text.split("\r\n")
    return tt


def parse_file(path: str) -> ThingType:
    import os
    stem = os.path.splitext(os.path.basename(path))[0]
    with open(path, "rb") as f:
        return parse_text(f.read().decode("latin1"), name=stem)


parse = parse_file


# --- offset-aware views ----------------------------------------------------
_ARRAY_RE = re.compile(r"^(weapon|child|path_point)\[(\d+)\](?:\.(name|w\d|c\d|x))?$")


def label_info(label: str) -> FieldInfo | None:
    """Map a parser label (``f1d4``, ``weapon[0].w2``, ...) to :class:`FieldInfo`."""
    for fi in FIELD_MAP:
        if fi.legacy == label or fi.soe == label:
            return fi
    m = _ARRAY_RE.match(label)
    if not m:
        return None
    kind, _idx, sub = m.groups()
    if kind == "path_point":
        return next(fi for fi in FIELD_MAP if fi.name == "path_point")
    if kind == "weapon":
        name = "weapon_id" if sub == "name" else "weapon_p" + sub[1:]
        return next(fi for fi in FIELD_MAP if fi.name == name)
    name = {"c0": "child_x", "c1": "child_y", "c2": "child_z",
            "c3": "child_r1", "c4": "child_r2", "c5": "child_r3",
            "x": "child_param"}[sub]
    return next(fi for fi in FIELD_MAP if fi.name == name)


def _fmt_value(value) -> str:
    if isinstance(value, float):
        return ("%.6g" % value)
    return str(value)


def _values_for(tt: ThingType, fi: FieldInfo) -> list[str]:
    """Values of *fi* as found in a parsed :class:`ThingType`."""
    if fi.name == "name":
        return [tt.name]
    if fi.name == "collision_kind":
        shape = str(tt.get("collision_shape", "")).lower()
        return [{"none": "0", "sphere": "1", "ellipsoid": "2"}.get(shape, "?")]
    if fi.name == "radius":
        shape = str(tt.get("collision_shape", "")).lower()
        if shape == "sphere":
            return [_fmt_value(tt.get("radius", "?"))]
        if shape == "ellipsoid":
            vals = [tt.get("radius_x"), tt.get("radius_y"), tt.get("radius_z")]
            if all(isinstance(v, (int, float)) for v in vals):
                return [_fmt_value(max(vals))]
        return ["?"]
    if fi.soe is None:
        return ["-"]
    if "[" in (fi.soe or ""):
        pat = re.compile("^" + re.escape(fi.soe).replace(r"\[i\]", r"\[\d+\]") + "$")
        vals = [f.value for f in tt.fields if pat.match(f.label)]
        if fi.count > 1:
            return [_fmt_value(v) for v in vals[:fi.count]] or ["-"]
        return [_fmt_value(v) for v in vals[:1]] or ["-"]
    f = next((f for f in tt.fields if f.label == fi.soe), None)
    if f is None:
        f = next((f for f in tt.fields if f.label == fi.legacy), None)
    return [_fmt_value(f.value)] if f is not None else ["-"]


def describe(tt: ThingType, verbose: bool = True) -> str:
    """Human-readable ``.soe`` -> record map with evidence/status.

    The first line is a summary, then one row per record field.  ``verbose``
    adds the consumer xrefs of every row.
    """
    lines = [
        "ThingType %r  version=%d  tokens=%d  fields=%d  extra=%d"
        % (tt.name or "?", tt.version, len(tt.fields) + tt.extra,
           len(tt.fields), tt.extra),
        "off     name             value                status      .soe token              role",
    ]
    for fi in FIELD_MAP:
        vals = _values_for(tt, fi)
        lines.append("0x%03X  %-16s %-20s %-11s %-22s %s"
                     % (fi.offset, fi.name, ",".join(vals)[:20], fi.status,
                        fi.soe or "-", fi.role))
        if verbose and fi.evidence:
            lines.append("        evidence: loader %s; xrefs %s"
                         % (fi.loader or "-", ", ".join(fi.evidence)))
    counts = {s: sum(1 for fi in FIELD_MAP if fi.status == s)
              for s in ("proven", "hypothesis", "unknown")}
    lines.append("summary: %d proven, %d hypothesis, %d unknown; "
                 "weapons=%s children=%s path=%s"
                 % (counts["proven"], counts["hypothesis"], counts["unknown"],
                    tt.get("weapon_count", "?"), tt.get("children_count", "?"),
                    tt.get("path_point_count", "?")))
    if tt.extra:
        lines.append("WARNING: %d trailing token(s) not consumed" % tt.extra)
    return "\n".join(lines)
