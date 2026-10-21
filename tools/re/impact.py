#!/usr/bin/env python3
"""Resolve the impact chain of a level's weapons straight from the ``.sbt``.

The chain the original engine walks (all addresses in ``gundx.exe`` 1.36):

``DEFINE_WEAPONS`` -> ``weapon[+0x30]`` (GENERATE event id, written at
``0x445eee``) -> the entity sub-script of that id -> the ``FIRE_SOURCEPLAYER_*``
condition ->

* ``FIRE_SOURCEPLAYER_THING`` (cid 20, ``0x45e19d`` -> ``0x46ea40``):
  operand ``G`` (index 2) is the ``THING_TYPES`` index.  The type record gives
  the thing's speed field ``+0x10`` (copied into the instance at ``0x46dd11``)
  and the expire/destroy/death event ids ``+0x1ec`` / ``+0x1f8`` / ``+0x1c4``.
  The impact burst is the ``CAUSE_BLAST_RADIUS_DAMAGE`` (op 75, ``0x45775b`` ->
  ``0x477280``) of the script the event id points at (possibly through
  ``GENERATE``).

* ``FIRE_SOURCEPLAYER_BULLET`` (cid 18) / ``FIRE_SOURCEPLAYER_LASER`` (cid 19):
  operand ``E`` (index 1 / 3) is the impact event id; the burst is the
  ``CAUSE_BLAST_RADIUS_DAMAGE`` of that script.

* ``ADD_THING_AT_SOURCE_WEAPON_POINT`` (cid 4) also carries a ``G`` operand and
  spawns the projectile (this is how ``RGM`` fires).

The burst itself is six operands ``LLLLLE`` (op 75): flags, no-direction,
plate, hull, radius, event id.  ``0x477280`` scales plate and hull by
``(radius - distance) / radius`` and posts ``event`` to every target.

Branch structure: the compiled script stores ``ELSE`` (op 200) as the *last*
instruction of the then-branch and the else-code *after* the ``IF``; the
``ELSE`` jump skips that else-code (``docs/VM_EXEC.md`` §1).  The resolver
therefore reconstructs the guards of every instruction.

Usage::

    python3 tools/re/impact.py [--game DIR] [--level STEM] [--json]
                               [--check] [--levels]

``--check`` verifies the invariants over every shipped level and exits
non-zero on a violation.  ``--json`` prints the resolution of one level.
"""

import argparse
import collections
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod            # noqa: E402
from gm import sbtvm                    # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# THING_TYPES record (type 4) field offsets (docs/SOE.md, gm/soe.py).
TT_SPEED = 0x10        # double -> instance+0x58 (0x46dd11); units per tick
TT_HITS = 0x1C8        # geometry hits before destroy (0x475615)
TT_EXPIRE = 0x1E8       # lifetime in ticks (the loader scales it by 0.91)
TT_EVENT_DEATH = 0x1C4
TT_EVENT_EXPIRE = 0x1EC
TT_EVENT_DESTROY = 0x1F8

# CAUSE_BLAST_RADIUS_DAMAGE (op 75) operand layout, proven at 0x45775b.
BLAST_FLAGS = 0
BLAST_NO_DIRECTION = 1
BLAST_PLATE = 2
BLAST_HULL = 3
BLAST_RADIUS = 4
BLAST_EVENT = 5

#: The engine's fixed tick (docs/PLAYER_GHIDRA.md, 0x44db80).
TICKS_PER_SECOND = 91.0

#: Guards that select the "normal" (not underwater) branch of a level script.
AIR_GUARD = "SOURCE_UNDERWATER"

MAX_EVENT_DEPTH = 4      # GENERATE hops followed inside one impact chain


def _s32(value):
    return value - 0x100000000 if value >= 0x80000000 else value


def _u32(tail, offset):
    return struct.unpack_from("<I", tail, offset - 0x198)[0]


class Level:
    """One parsed ``.sbt``: entity table, THING_TYPES and the weapon table."""

    def __init__(self, path):
        self.path = path
        data = sbtmod.parse_file(path)
        self.script = None
        self.things = []
        for record in data.records:
            if record.type == 11:
                self.script = record.payload
            elif record.type == 4:
                self.things = record.payload.entries
        if self.script is None:
            raise ValueError("%s: no SCRIPT record" % path)

        n = self.script.n
        cols = struct.unpack_from("<%dI" % (3 * n), self.script.handles, 0)
        # The engine looks an entity id up with a linear search over the id
        # column (0x457c49 / 0x45fa5c).  Several shipped levels carry a
        # second, DEATHMATCH copy of the same ids further down the table; the
        # first row is the campaign one.
        self.entities = {}
        for i in range(n):
            eid = _s32(cols[n + i])
            self.entities.setdefault(eid, cols[2 * n + i])
        self.offsets = sorted(set(cols[2 * n + i] for i in range(n)))
        self.weapons = self._weapon_table()
        self._cache = {}

    def _weapon_table(self):
        insns, _ = sbtvm.decode(self.script.code, 0, len(self.script.code))
        for insn in insns:
            if insn.name == "DEFINE_WEAPONS":
                return insn.data
        return []

    # -- entity scripts -------------------------------------------------
    def entity_script(self, eid):
        """Decoded instructions of the entity ``eid`` (first row wins)."""
        if eid in self._cache:
            return self._cache[eid]
        offset = self.entities.get(eid)
        if offset is None:
            self._cache[eid] = None
            return None
        end = self._block_end(offset)
        insns, _ = sbtvm.decode(self.script.code, offset, end)
        self._cache[eid] = insns
        return insns

    def _block_end(self, offset):
        nxt = [o for o in self.offsets if o > offset]
        return nxt[0] if nxt else len(self.script.code)

    # -- THING_TYPES ----------------------------------------------------
    def thing_type(self, index):
        if index is None or index < 0 or index >= len(self.things):
            return None
        entry = self.things[index]
        params = struct.pack("<49d", *entry.params)
        tail = entry.tail
        return {
            "index": index,
            "name": entry.name,
            "speed": struct.unpack_from("<d", params, TT_SPEED - 0x10)[0],
            "hits": _u32(tail, TT_HITS),
            "expire": _u32(tail, TT_EXPIRE),
            "event_death": _s32(_u32(tail, TT_EVENT_DEATH)),
            "event_expire": _s32(_u32(tail, TT_EVENT_EXPIRE)),
            "event_destroy": _s32(_u32(tail, TT_EVENT_DESTROY)),
        }


# ---------------------------------------------------------------------------
# branch reconstruction


def condition_of(insn):
    """The condition name of an ``IF``/``EVAL`` instruction (else ``None``)."""
    if insn.name not in ("IF", "EVAL"):
        return None
    if not insn.args:
        return None
    first = insn.args[0]
    if isinstance(first, tuple):
        return first[0]
    return None


def condition_args(insn):
    if insn.name not in ("IF", "EVAL") or not insn.args:
        return []
    first = insn.args[0]
    if isinstance(first, tuple):
        return list(first[1])
    return []


def walk(insns, guards=()):
    """Yield ``(instruction, guards)`` for every instruction of a block.

    ``guards`` is a tuple of ``(condition, taken)`` pairs, outermost first.
    ``taken`` is ``False`` for the else-code of the condition.
    """
    index = 0
    while index < len(insns):
        insn = insns[index]
        yield insn, guards

        condition = condition_of(insn)
        if condition is None:
            index += 1
            continue

        # then-branch
        for sub in walk(insn.body or [], guards + ((condition, True),)):
            yield sub

        # else-code: the code that follows the IF, up to the ELSE target
        # (the ELSE jump skips it when the condition holds).
        target = None
        if insn.body and insn.body[-1].name == "ELSE":
            target = insn.body[-1].args[0][1]
        if target is None and index + 1 < len(insns) and \
                insns[index + 1].name == "ELSE":
            target = insns[index + 1].args[0][1]
        if target is None:
            index += 1
            continue

        else_guards = guards + ((condition, False),)
        follow = index + 1
        if follow < len(insns) and insns[follow].name == "ELSE":
            # the sibling form: the ELSE carries the else-code itself
            for sub in walk(insns[follow].body or [], else_guards):
                yield sub
            follow += 1
        while follow < len(insns) and insns[follow].off < target:
            for sub in walk([insns[follow]], else_guards):
                yield sub
            follow += 1
        index = follow
    return


def is_underwater(guards):
    for name, taken in guards:
        if name == AIR_GUARD:
            return taken
    return False


def air(guards):
    """True when the guard chain selects the normal (not underwater) path."""
    return not is_underwater(guards)


# ---------------------------------------------------------------------------
# resolution


def collect_blasts(insns):
    """All ``CAUSE_BLAST_RADIUS_DAMAGE`` commands of a decoded block."""
    out = []
    for insn, _ in walk(insns):
        if insn.name != "CAUSE_BLAST_RADIUS_DAMAGE":
            continue
        args = insn.args
        out.append({
            "flags": args[BLAST_FLAGS],
            "no_direction": args[BLAST_NO_DIRECTION],
            "plate": args[BLAST_PLATE],
            "hull": args[BLAST_HULL],
            "radius": args[BLAST_RADIUS],
            "event": args[BLAST_EVENT],
        })
    return out


def generate_targets(insns):
    """Entity ids a block ``GENERATE``s (op 2)."""
    out = []
    for insn, _ in walk(insns):
        if insn.name == "GENERATE" and insn.args:
            out.append(_s32(insn.args[0]))
    return out


def impact_chain(level, start_event, depth=MAX_EVENT_DEPTH):
    """Bursts reachable from ``start_event`` through ``GENERATE`` hops."""
    blasts = []
    visited = []
    seen = set()

    def walk_event(eid, level_depth):
        if eid in seen or level_depth > depth:
            return
        seen.add(eid)
        visited.append(eid)
        insns = level.entity_script(eid)
        if insns is None:
            return
        blasts.extend(collect_blasts(insns))
        for target in generate_targets(insns):
            walk_event(target, level_depth + 1)

    walk_event(start_event, 0)
    return blasts, visited


def fire_branch(insns):
    """The ``FIRE_SOURCEPLAYER_*`` condition of a weapon script.

    The air branch wins over the ``SOURCE_UNDERWATER`` branch: the engine
    evaluates the conditions in order and the underwater body is guarded by
    ``SOURCE_UNDERWATER``, so an air shot never reaches it.  Returns
    ``(condition, args, guards)`` or ``None``.
    """
    best = None
    for insn, guards in walk(insns):
        condition = condition_of(insn)
        if condition is None or not condition.startswith("FIRE_SOURCEPLAYER_"):
            continue
        if best is None or (is_underwater(best[2]) and air(guards)):
            best = (condition, condition_args(insn), guards)
    return best


def resolve_weapon(level, record):
    """Resolve one ``DEFINE_WEAPONS`` record into its impact description."""
    strings, values = record
    name = strings[0]
    projectile = _s32(values[4])

    out = {
        "name": name,
        "projectile_event": projectile,
        "fire": None,
        "thing_type": None,
        "speed": None,
        "speed_per_second": None,
        "hits": None,
        "expire": None,
        "impact_events": [],
        "impacts": [],
        "visited": [],
    }

    insns = level.entity_script(projectile)
    if insns is None:
        return out

    branch = fire_branch(insns)
    if branch is not None:
        condition, args, guards = branch
        out["fire"] = condition

    # the thing type: FIRE_SOURCEPLAYER_THING and ADD_THING_AT_SOURCE_WEAPON_POINT
    # both carry the type in the G operand (index 2).
    type_index = None
    for insn, guards in walk(insns):
        condition = condition_of(insn)
        if condition in ("FIRE_SOURCEPLAYER_THING",
                         "ADD_THING_AT_SOURCE_WEAPON_POINT"):
            args = condition_args(insn)
            if len(args) > 2:
                type_index = _s32(args[2])
                break
    if type_index is not None:
        out["thing_type"] = type_index
        info = level.thing_type(type_index)
        if info is not None:
            out["speed"] = info["speed"]
            out["speed_per_second"] = info["speed"] * TICKS_PER_SECOND
            out["hits"] = info["hits"]
            out["expire"] = info["expire"]
            for key in ("event_expire", "event_destroy", "event_death"):
                eid = info[key]
                if eid:
                    out["impact_events"].append([key, eid])
    elif out["fire"] in ("FIRE_SOURCEPLAYER_BULLET", "FIRE_SOURCEPLAYER_LASER"):
        # hitscan: the E operand is the impact event id
        index = 1 if out["fire"] == "FIRE_SOURCEPLAYER_BULLET" else 3
        args = condition_args(insns) if branch is None else branch[1]
        if len(args) > index:
            out["impact_events"].append(["bullet_event", _s32(args[index])])

    # bursts of the weapon's own script (EMP GUN fires its rings there)
    for blast, guards in _blasts_with_guards(insns):
        out["impacts"].append({"blast": blast,
                              "guards": [[name, taken] for name, taken in guards]})

    for _, eid in out["impact_events"]:
        blasts, visited = impact_chain(level, eid)
        for blast in blasts:
            out["impacts"].append({"blast": blast, "guards": []})
        out["visited"].extend(visited)

    return out


def _blasts_with_guards(insns):
    out = []
    for insn, guards in walk(insns):
        if insn.name != "CAUSE_BLAST_RADIUS_DAMAGE":
            continue
        args = insn.args
        out.append(({
            "flags": args[BLAST_FLAGS],
            "no_direction": args[BLAST_NO_DIRECTION],
            "plate": args[BLAST_PLATE],
            "hull": args[BLAST_HULL],
            "radius": args[BLAST_RADIUS],
            "event": args[BLAST_EVENT],
        }, guards))
    return out


def resolve_level(path):
    level = Level(path)
    return [resolve_weapon(level, record) for record in level.weapons]


# ---------------------------------------------------------------------------
# checks


def check(path):
    """Invariants over one level.  Returns a list of error strings."""
    level = Level(path)
    errors = []
    seen_types = collections.Counter()
    for record in level.weapons:
        info = resolve_weapon(level, record)
        name = info["name"]
        if info["fire"] is None:
            errors.append("%s: no FIRE_SOURCEPLAYER_* branch" % name)
            continue
        if info["thing_type"] is not None:
            index = info["thing_type"]
            seen_types[index] += 1
            if index < 0 or index >= len(level.things):
                errors.append("%s: thing type %r out of range" % (name, index))
                continue
            if info["speed"] is None or info["speed"] < 0.0:
                errors.append("%s: bad speed %r" % (name, info["speed"]))
        for _, eid in info["impact_events"]:
            if eid not in level.entities:
                errors.append("%s: impact event %d not in the entity table"
                              % (name, eid))
        for impact in info["impacts"]:
            if impact["blast"]["radius"] < 0:
                errors.append("%s: negative radius %r"
                              % (name, impact["blast"]["radius"]))
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game", default=DEFAULT_GAME, help="game install dir")
    ap.add_argument("--level", default="level0", help="level stem (e.g. level0)")
    ap.add_argument("--json", action="store_true", help="print JSON")
    ap.add_argument("--check", action="store_true", help="check all levels")
    ap.add_argument("--levels", action="store_true", help="list levels")
    args = ap.parse_args(argv)

    if args.levels:
        for name in sorted(os.listdir(args.game)):
            if name.endswith(".sbt"):
                print(os.path.splitext(name)[0])
        return 0

    if args.check:
        failures = 0
        total = 0
        for name in sorted(os.listdir(args.game)):
            if not name.endswith(".sbt"):
                continue
            total += 1
            errors = check(os.path.join(args.game, name))
            if errors:
                failures += 1
                for err in errors:
                    print("%s: %s" % (name, err))
        print("checked %d level(s), %d with errors" % (total, failures))
        return 1 if failures else 0

    path = os.path.join(args.game, args.level + ".sbt")
    resolved = resolve_level(path)
    if args.json:
        print(json.dumps(resolved, indent=2, sort_keys=True))
        return 0

    for info in resolved:
        print("%-16s fire=%-26s" % (info["name"], info["fire"]))
        if info["thing_type"] is not None:
            print("    thing=%d speed=%s u/tick (%s u/s) hits=%s expire=%s"
                  % (info["thing_type"], info["speed"], info["speed_per_second"],
                     info["hits"], info["expire"]))
        for key, eid in info["impact_events"]:
            print("    %-14s -> entity %d" % (key, eid))
        if info["visited"]:
            print("    chain: %s" % ", ".join(str(v) for v in info["visited"]))
        for impact in info["impacts"]:
            blast = impact["blast"]
            print("    blast plate=%s hull=%s radius=%s flags=%s event=%s guards=%s"
                  % (blast["plate"], blast["hull"], blast["radius"],
                     blast["flags"], blast["event"], impact["guards"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
