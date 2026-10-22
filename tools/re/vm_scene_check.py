#!/usr/bin/env python3
"""VM-created scene vs the scene the level file describes (``M5.2``).

For every shipped ``.sbt`` the tool runs the ``SCRIPT`` VM
(:mod:`gm.sbtvm_exec`) over each entity sub-script and compares what the VM
built with the static extraction of :mod:`gm.scene`:

* **placed objects** -- lights, locations, items, sounds, sprites, doors,
  switches, lifts and player spawns: multisets of the fields that identify
  them (position, ids, radii, ...), plus the code offsets that produced them;
* **things** -- every ``ADD_THING*`` site.  The static side is
  ``gm.scene.things(..., recursive=True)`` plus the things the level uses
  *directly as an ``IF`` condition* (``IF ADD_THING_...``, opcode 199), which
  the static walker does not collect.  Each site's semantics are compared
  (kind, type index, position, owner) and the type is resolved through
  ``THING_TYPES`` -> ``SOB_TABLE`` -> model;
* **level exits** -- the ``NEW_LEVEL`` / ``NEXT_LEVEL`` / ``END_GAME`` sites
  of the level against the transition table of ``docs/FORMATS.md`` 15.

The VM is run in several runtime contexts (campaign / deathmatch classic,
deterministic and random-explored, player source, hard difficulty, complete
objectives).  A statically described site that none of them reaches is re-run
with a context derived from its ``IF`` chain (``GAME_DIFFICULTY_AT_LEAST 4``,
``LEVEL_OBJECTIVES_COMPLETE``, ``SOURCE_IS_PLAYER``, ...), so a miss is
reported only where the model itself cannot reach the site.

Usage::

    python3 tools/re/vm_scene_check.py --summary
    python3 tools/re/vm_scene_check.py --json > scene.json
    python3 tools/re/vm_scene_check.py --level dm1 --detail
    python3 tools/re/vm_scene_check.py --self-test     # no game files needed

``.sbt`` files live outside the repository; ``GUNMETAL_GAME`` or ``--game``
selects the installation (default ``/mnt/d/games/Gunmetal 3dfx``).
"""
import argparse
import ast
import bisect
import collections
import glob
import json
import os
import re
import struct
import sys
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod            # noqa: E402
from gm import scene as scenemod       # noqa: E402
from gm import sbtvm                   # noqa: E402
from gm import sbtvm_exec as vm        # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
FORMATS = os.path.join(ROOT, "docs", "FORMATS.md")

#: ``gm.scene`` categories compared object by object (things have their own
#: section because the static and the runtime views are built differently)
SCENE_CATEGORIES = ("lights", "locations", "items", "sounds", "sprites",
                    "doors", "switches", "lifts", "spawns")

#: ``THING_TYPES`` entry field ``+0x1a4``: index into ``SOB_TABLE``
MODEL_OFF = 0x1A4

#: ``docs/FORMATS.md`` 15 spells the condition shorthand ``[DM]``
DOC_CONDITIONS = {"DM": "DEATHMATCH_GAME"}


@dataclass(frozen=True)
class Context:
    """One runtime context of the VM.

    The same fields as :class:`gm.scene.ThingContext` plus the two the level
    counter needs (``level_number``, ``mp_flags``), which
    :func:`gm.sbtvm_exec.state_from_script` accepts but ``ThingContext`` does
    not carry.
    """

    game_mode: int = 0            # 0 campaign, 1 dm-normal, 2 dm-classic, 5 scavenger
    difficulty: int = 1
    players: int = 1
    level_number: int = 0
    mp_flags: int = 0
    playertype: int | None = None
    source_player: int | None = None
    god_mode: bool = False
    objectives_complete: bool = False
    seed: int = 0x12345678
    random_unknown: bool = False
    max_steps: int = 200_000
    max_time: float = 5.0
    max_paths: int = 64
    on_unknown: str = "both"
    follow_generate: bool = False

    def label(self):
        bits = ["mode=%d" % self.game_mode, "diff=%d" % self.difficulty,
                "players=%d" % self.players]
        if self.level_number:
            bits.append("level_nr=%d" % self.level_number)
        if self.mp_flags:
            bits.append("mp=%d" % self.mp_flags)
        if self.playertype is not None:
            bits.append("type=%d" % self.playertype)
        if self.source_player is not None:
            bits.append("src_player=%d" % self.source_player)
        if self.god_mode:
            bits.append("god")
        if self.objectives_complete:
            bits.append("objectives")
        if self.random_unknown:
            bits.append("random=explore")
        return ",".join(bits)


#: runtime contexts each level is executed in
CONTEXTS = (
    Context(),                                                # campaign
    Context(game_mode=2),                                     # DM classic
    Context(random_unknown=True),
    Context(game_mode=2, random_unknown=True),
    Context(source_player=0),
    Context(game_mode=2, source_player=0),
    Context(difficulty=3),
    Context(objectives_complete=True),
)

EXIT_NAMES = ("NEW_LEVEL", "NEXT_LEVEL", "END_GAME", "NEW_GAME")


# --------------------------------------------------------------------------
# generic helpers


def level_files(game):
    return sorted(glob.glob(os.path.join(game, "*.sbt")))


def load_script(path):
    f = sbtmod.parse_file(path)
    recs = f.records_of(11)
    if not recs:
        raise SystemExit("%s: no SCRIPT record" % path)
    return f, recs[0].payload


def s32(v):
    return v - 0x100000000 if isinstance(v, int) and v >= 0x80000000 else v


def uid(v):
    return v & 0xFFFFFFFF


def block_offsets(script):
    """``(entity id, block offset)`` in table order, one row per block."""
    out = []
    seen = set()
    for eid, _flags, off in scenemod.entities(script):
        if off not in seen and 0 <= off < len(script.code):
            seen.add(off)
            out.append((eid, off))
    return out


def walk_all(insns):
    """``(insn, path)`` of a decoded tree, recursing into ``IF``/``ELSE``.

    Mirrors :func:`tools/re/campaign._walk`: ``0xFF`` is an ordinary
    instruction here, so a linear decode of the whole code visits every
    sub-script of the level (this is the view ``gm.scene.extract`` and the
    exit table use).
    """
    for ins in insns:
        yield ins, ()
        body = getattr(ins, "body", None)
        if not isinstance(body, list):
            continue
        label = ("ELSE",) if ins.name == "ELSE" else (
            "IF", ins.args[0][0], tuple(ins.args[0][1])) if (
                ins.name == "IF" and ins.args) else (ins.name,)
        for sub, path in _walk_nested(body):
            yield sub, (label,) + path


def _walk_nested(insns, path=()):
    for ins in insns:
        yield ins, path
        body = getattr(ins, "body", None)
        if not isinstance(body, list):
            continue
        label = ("ELSE",) if ins.name == "ELSE" else (
            "IF", ins.args[0][0], tuple(ins.args[0][1])) if (
                ins.name == "IF" and ins.args) else (ins.name,)
        for sub, subpath in _walk_nested(body, (label,) + path):
            yield sub, subpath


def walk_block(insns):
    """``(insn, path)`` of one sub-script, stopping at the first ``0xFF``.

    ``0xFF`` ends the whole sub-script, not just the current ``IF`` body --
    the rule :func:`gm.scene._walk_things` uses for the static thing walk and
    the one the executor follows when its sub-script stack is empty.
    """
    out = []
    _collect_block(insns, (), out)
    return out


def _collect_block(insns, path, out):
    for ins in insns:
        out.append((ins, path))
        if ins.name == "END":
            return True
        body = getattr(ins, "body", None)
        if not isinstance(body, list):
            continue
        label = ("ELSE",) if ins.name == "ELSE" else (
            "IF", ins.args[0][0], tuple(ins.args[0][1])) if (
                ins.name == "IF" and ins.args) else (ins.name,)
        if _collect_block(body, (label,) + path, out):
            return True
    return False


def path_labels(path):
    """Human labels of a guard chain, outermost first."""
    out = []
    for step in reversed(path):
        if step[0] == "ELSE":
            out.append("ELSE")
        elif step[0] == "IF":
            out.append("IF %s %s" % (step[1], list(step[2])))
        else:
            out.append(step[0])
    return out


def objectives_of(script):
    """``DEFINE_LEVEL_OBJECTIVES`` list of the level (main block)."""
    for ins, _path in walk_all(_whole_code(script)):
        if ins.name == "DEFINE_LEVEL_OBJECTIVES" and isinstance(ins.args, list):
            return [a for a in ins.args if isinstance(a, str)]
    return []


def _whole_code(script):
    ins, _end = sbtvm.decode(script.code)
    return ins


def thing_models(f):
    """``(type names, SOB model names)`` of a parsed ``.sbt``.

    The same resolution as ``tools/re/thing_models.py`` and
    ``gm/scene.py:_thing_model``: ``THING_TYPES`` record, tail field ``+0x1a4``.
    """
    if f is None:
        return [], []
    names = []
    models = []
    try:
        names = [t.name for t in f.records_of(4)[0].payload.entries]
    except Exception:                                  # noqa: BLE001
        pass
    try:
        models = [e.name for e in f.records_of(20)[0].payload.entries]
    except Exception:                                  # noqa: BLE001
        pass
    return names, models


def model_of(f, ti, names, models):
    """``(type name, model name or None)`` for a ``THING_TYPES`` index."""
    if not 0 <= ti < len(names):
        return None, None
    try:
        tt = f.records_of(4)[0].payload.entries[ti]
        vals = struct.unpack_from("<%dI" % (len(tt.tail) // 4), tt.tail)
        idx = vals[(MODEL_OFF - 0x198) // 4]
    except Exception:                                  # noqa: BLE001
        return names[ti], None
    return names[ti], (models[idx] if 0 <= idx < len(models) else None)


def rfl(v):
    """Round a float so equal values compare (and serialise) equal."""
    return round(float(v), 6)


# --------------------------------------------------------------------------
# object identity


def object_key(cat, o):
    """The fields that identify one placed object (compared as a multiset)."""
    if cat == "lights":
        return (o.kind, tuple(s32(v) for v in o.pos),
                tuple(int(v) for v in o.rgb), s32(o.param))
    if cat == "locations":
        return (s32(o.thing_id), tuple(s32(v) for v in o.pos))
    if cat == "items":
        return (s32(o.item_id), tuple(s32(v) for v in o.pos), bool(o.fixed))
    if cat == "sounds":
        return (tuple(s32(v) for v in o.pos), s32(o.sample), s32(o.volume),
                rfl(o.pitch))
    if cat == "sprites":
        return (tuple(rfl(v) for v in o.pos), s32(o.jitter), s32(o.id))
    if cat == "doors":
        return (s32(o.flags), tuple(s32(v) for v in o.ids),
                tuple(rfl(v) for v in o.angles),
                tuple(s32(v) for v in o.params))
    if cat == "switches":
        return (s32(o.kind), s32(o.id), s32(o.radius),
                tuple(s32(v) for v in o.pos))
    if cat == "lifts":
        return (bytes(o.head), tuple(tuple(v) for v in o.stops))
    if cat == "spawns":
        return (s32(o.index), tuple(s32(v) for v in o.pos),
                tuple(rfl(v) for v in o.angles))
    raise KeyError(cat)


def key_json(key):
    """A JSON-friendly rendering of an :func:`object_key` tuple."""
    return [list(k) if isinstance(k, tuple) else k for k in key]


def static_scene(f):
    return scenemod.from_sbt(f)


# --------------------------------------------------------------------------
# static side: things and exits


def static_things(script, f):
    """``(site -> Thing, recursive count, IF-condition count)``.

    ``gm.scene.things(recursive=True)`` walks the ``EVAL ADD_THING*`` sites of
    every entity block (including ``IF``/``ELSE`` bodies).  A thing can also
    be the *condition* of an ``IF`` (opcode 199, ``IF ADD_THING_...(...)``):
    the engine creates it while evaluating the condition, so it is part of the
    level, but the static walker never looks at ``IF`` operands.
    """
    out = {}
    recursive = scenemod.things(script, f, recursive=True)
    for th in recursive:
        out.setdefault(th.site, th)
    names, models = thing_models(f)
    added = 0
    for eid, off in block_offsets(script):
        try:
            ins, _end = sbtvm.decode(script.code, off, len(script.code))
        except Exception:                              # noqa: BLE001
            continue
        for i, path in walk_block(ins):
            if i.name != "IF" or not i.args:
                continue
            if not str(i.args[0][0]).startswith("ADD_THING"):
                continue
            try:
                th = scenemod._thing_from(i, eid, names, models, f,
                                          path=path_labels(path),
                                          depth=len(path), via="static-if")
            except Exception:                          # noqa: BLE001
                continue
            if th is not None and th.site not in out:
                added += 1
                out.setdefault(th.site, th)
    return out, len(recursive), added


def static_exits(script):
    """Every ``NEW_LEVEL``/``NEXT_LEVEL``/``END_GAME`` site of the level."""
    out = {}
    for ins, path in walk_all(_whole_code(script)):
        if ins.name not in EXIT_NAMES:
            continue
        target = None
        for a in ins.args or ():
            if isinstance(a, str):
                target = a
                break
        out[ins.off] = {"pc": ins.off, "name": ins.name, "target": target,
                        "conds": path_labels(path)}
    return out


def static_level_name(script):
    for ins, _path in walk_all(_whole_code(script)):
        if ins.name == "SET_LEVEL_NAME" and ins.args:
            return ins.args[0] if isinstance(ins.args[0], str) else None
    return None


# --------------------------------------------------------------------------
# runtime side


def state_for(script, ctx):
    st = vm.state_from_script(
        script, game_mode=ctx.game_mode, player_count=ctx.players,
        difficulty=ctx.difficulty, players=ctx.players, seed=ctx.seed,
        playertype=ctx.playertype, source_player=ctx.source_player,
        god_mode=ctx.god_mode)
    if ctx.objectives_complete:
        objs = objectives_of(script)
        st.objectives = list(objs)
        st.objectives_bits = (1 << len(objs)) - 1 if objs else 0
    return st


def parse_transition(result):
    """``(name, target)`` of a ``LEVEL_TRANSITION`` halt."""
    m = re.match(r"^(\w+) (.*)$", result.detail or "")
    if not m:
        return None, None
    name, rest = m.group(1), m.group(2)
    try:
        args = ast.literal_eval(rest)
    except (ValueError, SyntaxError):
        args = ()
    target = args[0] if isinstance(args, (list, tuple)) and args else None
    return name, target


def run_context(script, ctx, executor=None, only=None):
    """Execute entity blocks in *ctx*; return the merged scene.

    *only* restricts the run to the given ``(eid, offset)`` rows (used by the
    reachability probe).
    """
    st = state_for(script, ctx)
    ex = executor or vm.Executor(
        st, max_steps=ctx.max_steps, max_time=ctx.max_time,
        max_paths=ctx.max_paths, on_unknown=ctx.on_unknown,
        follow_generate=ctx.follow_generate,
        random_unknown=ctx.random_unknown)
    scene = vm.Scene()
    seen = set()
    exits = {}
    reasons = collections.Counter()
    paths = 0
    for eid, off in (block_offsets(script) if only is None else only):
        results = ex.run(off, source_entity=eid)
        paths += len(results)
        for r in results:
            reasons[r.reason] += 1
            if r.reason == vm.LEVEL_TRANSITION:
                name, target = parse_transition(r)
                exits.setdefault(r.pc, {"pc": r.pc, "name": name,
                                        "target": target})
        merged = vm.merge_scenes(results)
        for cat in scene.counts():
            for obj in getattr(merged, cat):
                key = (cat, getattr(obj, "site", None),
                       getattr(obj, "occ", None))
                if key in seen:
                    continue
                seen.add(key)
                getattr(scene, cat).append(obj)
    return scene, exits, reasons, paths, list(ex.notes)


def run_level(script):
    """Union of every context: ``(scene, exits, reasons, paths, notes)``.

    Objects created at the same ``(site, occurrence)`` in two contexts are the
    same scene item, so they are collapsed -- the merge rule
    :func:`gm.sbtvm_exec.merge_scenes` uses inside one context.
    """
    scene = vm.Scene()
    seen = set()
    exits = {}
    reasons = collections.Counter()
    notes = []
    paths = 0
    for ctx in CONTEXTS:
        part, part_exits, part_reasons, part_paths, part_notes = run_context(
            script, ctx)
        for cat in scene.counts():
            for obj in getattr(part, cat):
                key = (cat, getattr(obj, "site", None),
                       getattr(obj, "occ", None))
                if key in seen:
                    continue
                seen.add(key)
                getattr(scene, cat).append(obj)
        exits.update(part_exits)
        reasons.update(part_reasons)
        paths += part_paths
        notes.extend(n for n in part_notes if n not in notes)
    return scene, exits, reasons, paths, notes


# --------------------------------------------------------------------------
# reachability probe


def locate(script, site):
    """``(block offset, guard path)`` of a code offset."""
    starts = sorted({off for _eid, off in block_offsets(script)})
    if not starts:
        return 0, ()
    block = starts[max(0, bisect.bisect_right(starts, site) - 1)]
    try:
        ins, _end = sbtvm.decode(script.code, block, len(script.code))
    except Exception:                                  # noqa: BLE001
        return block, ()
    stack = [(ins, ())]
    while stack:
        insns, path = stack.pop()
        for i in insns:
            if i.off == site:
                return block, path
            body = getattr(i, "body", None)
            if not isinstance(body, list):
                continue
            label = ("ELSE",) if i.name == "ELSE" else (
                "IF", i.args[0][0], tuple(i.args[0][1])) if (
                    i.name == "IF" and i.args) else (i.name,)
            stack.append((body, (label,) + path))
    return block, ()


def context_from_chain(chain):
    """The narrowest context that satisfies the guards of *chain*.

    Only *known* conditions are a problem for the executor (unknown ones are
    forked by ``on_unknown="both"``), so only those are turned into state.
    Returns ``(ctx, unresolved)``.
    """
    mode = 0
    difficulty = 1
    players = 1
    level_number = 0
    mp_flags = 0
    god = False
    objectives = False
    source_player = None
    random_unknown = False
    unresolved = []
    for step in chain:
        if step[0] != "IF":
            continue
        name, args = step[1], list(step[2])
        arg = s32(args[0]) if args and isinstance(args[0], int) else None
        if name == "GAME_DIFFICULTY_AT_LEAST" and arg is not None:
            difficulty = max(difficulty, arg)
        elif name == "LEVEL_OBJECTIVES_COMPLETE":
            objectives = True
        elif name == "SOURCE_IS_PLAYER":
            source_player = 0
        elif name == "SOURCE_IS_LOCAL_PLAYER_ONE":
            source_player = 0
        elif name == "SOURCE_IS_LOCAL_PLAYER_TWO":
            source_player = 1
        elif name in ("SOURCEPLAYER_IS",) and arg is not None:
            source_player = arg
        elif name == "DEATHMATCH_CLASSIC_GAME":
            mode = 2
        elif name == "DEATHMATCH_NORMAL_GAME":
            mode = 1
        elif name == "DEATHMATCH_GAME":
            mode = 2
        elif name == "SCAVENGERHUNT_GAME":
            mode = 5
        elif name == "MULTIPLAYER_GAME":
            mode, players = 1, max(players, 2)
        elif name == "CAPTURE_THE_FLAG_GAME":
            mp_flags |= 2
        elif name == "TEAM_GAME":
            mp_flags |= 1
        elif name == "GOD_MODE":
            god = True
        elif name == "NEXT_LEVEL_NUMBER" and arg is not None:
            level_number = arg
        elif name == "PLAYERS_AT_LEAST" and arg is not None:
            players = max(players, arg)
        elif name in ("RANDOM", "RANDOM_FRAME_BALANCED"):
            random_unknown = True
        else:
            unresolved.append(name)
    ctx = Context(
        game_mode=mode, difficulty=difficulty, players=players,
        level_number=level_number, mp_flags=mp_flags, god_mode=god,
        objectives_complete=objectives, source_player=source_player,
        random_unknown=random_unknown)
    return ctx, unresolved


def probe_sites(script, sites):
    """Try to reach *sites* with a context built from their guard chains."""
    reached = []
    unreachable = []
    rows = {off: eid for eid, off in block_offsets(script)}
    for site in sorted(sites):
        block, chain = locate(script, site)
        ctx, unresolved = context_from_chain(chain)
        scene, _exits, _reasons, _paths, _notes = run_context(
            script, ctx, only=[(rows.get(block), block)])
        got = {getattr(t, "site", None) for t in scene.things}
        row = {"site": site, "block": block, "entity": rows.get(block),
               "context": ctx.label(), "conds": path_labels(chain)}
        if site in got:
            reached.append(row)
        else:
            row["unresolved"] = unresolved
            unreachable.append(row)
    return reached, unreachable


# --------------------------------------------------------------------------
# docs/FORMATS.md 15


def docs_table(path=FORMATS):
    """``stem -> {name, exits:[(target, cond)]}`` from ``FORMATS.md`` 15."""
    try:
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return {}
    start = None
    for i, line in enumerate(lines):
        if line.startswith("| Уровень | Имя | Секретов | Лучшее время"):
            start = i
            break
    if start is None:
        return {}
    out = {}
    for line in lines[start + 1:]:
        if not line.startswith("|"):
            break
        cells = [c.strip() for c in line.split("|")]
        if len(cells) < 7:
            continue
        m = re.match(r"`([a-z0-9]+)`", cells[1])
        if not m:
            continue
        exits = []
        for token in re.findall(r"`([^`]+)`", cells[5]):
            mm = re.match(r"^(\S+?)(?:\s+\[([^\]]+)\])?$", token.strip())
            if not mm:
                continue
            cond = mm.group(2)
            exits.append((mm.group(1),
                          DOC_CONDITIONS.get(cond, cond) if cond else None))
        out[m.group(1)] = {"name": cells[2].strip("`"),
                           "exits": exits, "secrets": cells[3],
                           "best": cells[4]}
    return out


def exit_key(name, target, conds):
    """Normalised ``(target, condition)`` of one exit site."""
    tgt = target if name == "NEW_LEVEL" else name
    cond = None
    if conds:
        cond = "+".join(c.replace("IF ", "").split(" ")[0]
                        for c in conds if c != "ELSE")
        cond = cond or None
    return ((tgt or "").lower(), (cond or "").lower() or None)


# --------------------------------------------------------------------------
# one level


def check_level(path=None, *, script=None, f=None, docs=None, label=None):
    """Compare the VM scene and the static scene of one level."""
    if script is None:
        f, script = load_script(path)
        label = label or os.path.basename(path)[:-4]
    docs = docs if docs is not None else docs_table()
    row = {"level": label, "blocks": len(block_offsets(script)),
           "paths": 0, "notes": []}

    base = static_scene(f) if f is not None else scenemod.extract(script)
    vm_scene, exits, reasons, paths, notes = run_level(script)
    row["paths"] = paths
    row["reasons"] = {k: v for k, v in sorted(reasons.items())}
    row["notes"] = notes[:12]

    # -- placed objects ----------------------------------------------------
    scene = {}
    scene_ok = True
    for cat in SCENE_CATEGORIES:
        want = collections.Counter(object_key(cat, o)
                                   for o in getattr(base, cat))
        have = collections.Counter(object_key(cat, o)
                                   for o in getattr(vm_scene, cat))
        missing = list((want - have).elements())
        extra = list((have - want).elements())
        entry = {"static": sum(want.values()), "vm": sum(have.values())}
        if missing:
            entry["missing"] = [key_json(k) for k in missing[:8]]
        if extra:
            entry["extra"] = [key_json(k) for k in extra[:8]]
        if missing or extra:
            scene_ok = False
            # code offsets: every static command site vs the executed ones
            static_sites = _category_sites(script, cat)
            exec_sites = {getattr(o, "site", None)
                          for o in getattr(vm_scene, cat)}
            lost = sorted(static_sites - exec_sites)
            if lost:
                entry["sites"] = [hex(s) for s in lost[:8]]
        scene[cat] = entry
    row["scene"] = scene
    row["scene_exact"] = scene_ok

    # -- things ------------------------------------------------------------
    described, rec_count, if_cond = static_things(script, f)
    names, models = thing_models(f)
    stat_top = {th.site for th in scenemod.things(script, f)}
    exec_things = {}
    for th in vm_scene.things:
        exec_things.setdefault(th.site, th)
    missing = sorted(set(described) - set(exec_things))
    extra = sorted(set(exec_things) - set(described))
    field_diff = []
    unresolved = 0
    inherited = 0
    for site in sorted(set(described) & set(exec_things)):
        a = described[site]
        b = exec_things[site]
        if (a.kind, s32(a.type_index), a.pos, s32(a.owner)) != \
                (b.kind, s32(b.type_index), b.pos, s32(b.owner)):
            field_diff.append({
                "site": site, "static": [a.kind, s32(a.type_index),
                                         list(a.pos or ()), s32(a.owner)],
                "vm": [b.kind, s32(b.type_index), list(b.pos or ()),
                       s32(b.owner)]})
        ti = s32(b.type_index)
        if ti < 0:
            # ``ADD_THING_AT_SOURCE`` with type -1: the thing is whatever the
            # generated entity is, so the file names no type at all
            inherited += 1
        elif f is not None and model_of(f, ti, names, models)[1] is None:
            unresolved += 1
    reached, unreachable = probe_sites(script, missing) if missing else ([], [])
    still = [r["site"] for r in unreachable]
    row["things"] = {
        "static_top": len(stat_top),
        "static_recursive": rec_count,
        "static_conditions": if_cond,
        "static_total": len(described),
        "vm": len(exec_things),
        "vm_reachable": len(exec_things) + len(reached),
        "vm_probed": len(reached),
        "if_condition_sites": if_cond,
        "unresolved_types": unresolved,
        "inherited_types": inherited,
    }
    if extra:
        row["things"]["extra"] = [{"site": s,
                                   "kind": exec_things[s].kind,
                                   "type": s32(exec_things[s].type_index)}
                                  for s in extra[:8]]
    if missing:
        row["things"]["missing"] = [{"site": s,
                                     "kind": described[s].kind,
                                     "type": s32(described[s].type_index),
                                     "conds": list(described[s].path or ())}
                                    for s in missing[:16]]
    if field_diff:
        row["things"]["field_mismatch"] = field_diff[:8]
    if reached:
        row["things"]["reachable"] = reached[:16]
    if still:
        row["things"]["unreachable"] = unreachable[:16]
    row["things_exact"] = (not extra and not field_diff and not still
                           and unresolved == 0
                           and row["things"]["vm_reachable"] ==
                           row["things"]["static_total"])

    # -- exits -------------------------------------------------------------
    st_exits = static_exits(script)
    vm_pcs = set(exits)
    st_pcs = set(st_exits)
    doc = docs.get(label)
    doc_set = {(t.lower(), (c or "").lower() or None)
               for t, c in doc["exits"]} if doc else set()
    st_set = {exit_key(st_exits[p]["name"], st_exits[p]["target"],
                       st_exits[p]["conds"]) for p in st_pcs}
    target_mismatch = [
        {"pc": p, "static": st_exits[p]["target"], "vm": exits[p]["target"],
         "name": st_exits[p]["name"]}
        for p in sorted(st_pcs & vm_pcs)
        if st_exits[p]["target"] != exits[p]["target"]
        or st_exits[p]["name"] != exits[p]["name"]]
    row["exits"] = {
        "static": [{"pc": p, "name": st_exits[p]["name"],
                    "target": st_exits[p]["target"],
                    "conds": st_exits[p]["conds"]} for p in sorted(st_pcs)],
        "vm": [{"pc": p, "name": exits[p]["name"],
                "target": exits[p]["target"]} for p in sorted(vm_pcs)],
        "static_missing": [{"pc": p, "name": st_exits[p]["name"],
                            "target": st_exits[p]["target"],
                            "conds": st_exits[p]["conds"]}
                           for p in sorted(st_pcs - vm_pcs)],
        "vm_extra": [{"pc": p, "name": exits[p]["name"],
                      "target": exits[p]["target"]}
                     for p in sorted(vm_pcs - st_pcs)],
        "target_mismatch": target_mismatch[:8],
        "docs": [{"target": t, "cond": c} for t, c in sorted(doc_set)],
        "docs_vs_static": sorted(
            (dict(zip(("target", "cond"), a)) for a in (st_set ^ doc_set)),
            key=lambda d: (d["target"] or "", d["cond"] or "")),
        "docs_ok": st_set == doc_set,
    }
    row["exits_exact"] = (st_pcs == vm_pcs and not target_mismatch)
    row["level_name"] = static_level_name(script)
    row["docs_name"] = doc["name"] if doc else None
    row["docs_name_ok"] = (
        row["docs_name"] is None or row["level_name"] is None
        or row["docs_name"].strip().lower() ==
        row["level_name"].strip().lower())
    row["ok"] = (row["scene_exact"] and row["things_exact"]
                 and row["exits_exact"])
    return row


def _category_sites(script, cat):
    """Code offsets of every statically decoded command of *cat*."""
    opcodes = {
        "lights": ("ADD_SKY_LIGHT_SOURCE", "ADD_LIGHT_SOURCE"),
        "locations": ("ADD_LOCATION", "ADD_LOCATION_EX"),
        "items": ("ADD_ITEM", "ADD_ITEM_FIXED", "ADD_NAMED_ITEM",
                  "ADD_NAMED_ITEM_EX"),
        "sounds": ("ADD_SOUND_SOURCE",),
        "sprites": ("ADD_SPRITE",),
        "doors": ("ADD_DOOR",),
        "switches": ("ADD_SWITCH",),
        "lifts": ("ADD_LIFT",),
        "spawns": ("SET_PLAYER_START", "SET_DEATHMATCH_START"),
    }.get(cat, ())
    out = set()
    for ins, _path in walk_all(_whole_code(script)):
        if ins.name in opcodes:
            out.add(ins.off)
    return out


# --------------------------------------------------------------------------
# self-test


def _enc(name, *args):
    """Encode one command / ``EVAL`` condition (synthetic sub-scripts)."""
    if name in {v[0] for v in sbtvm.COMMANDS.values()}:
        op = next(k for k, v in sbtvm.COMMANDS.items() if v[0] == name)
        out = bytearray([op])
        sig = sbtvm.COMMANDS[op][1] or ""
    else:
        cid = next(k for k, v in sbtvm.CONDITIONS.items() if v[0] == name)
        out = bytearray([0xC9, cid])
        sig = sbtvm.CONDITIONS[cid][1] or ""
    for ch, v in zip(sig, args):
        size = sbtvm.LETTER_SIZE[ch]
        if size == "str":
            out += str(v).encode("latin1") + b"\0"
        elif size == 8:
            out += struct.pack("<d", float(v))
        elif size == 1:
            out += bytes([int(v) & 0xFF])
        else:
            out += struct.pack("<I", int(v) & 0xFFFFFFFF)
    return bytes(out)


def _synthetic_script(*, extra_thing=False):
    """A level with two things, one door and a ``NEW_LEVEL`` exit."""
    body = bytearray()
    body += _enc("ADD_THING", 10, 100, 200, 300, 0.0, 0.0, 0.0)
    body += _enc("ADD_THING", 11, -100, 200, 300, 0.0, 0.0, 1.0)
    if extra_thing:
        # a thing only built when the level objectives are complete -- the
        # static walker counts it, the VM cannot reach it in any context
        # (the synthetic level defines no objectives)
        inner = _enc("ADD_THING", 12, 400, 500, 600, 0.0, 0.0, 0.0)
        cid = next(k for k, v in sbtvm.CONDITIONS.items()
                   if v[0] == "LEVEL_OBJECTIVES_COMPLETE")
        body += bytes([199, cid]) + struct.pack("<I", 4 + len(inner)) + inner
    body += _enc("ADD_DOOR", 2, 900, 0, 0, 0, 0,
                 0.0, 0.0, 0.0,
                 1, 2, 3, 4, 5, 6, 7,
                 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 10.0, 10.0, 10.0,
                 0, 1, 120, 700, 1500, 99999)
    body += b"\xff"
    exit_block = _enc("NEW_LEVEL", "level2") + b"\xff"
    code = bytes(body) + exit_block
    off_exit = len(body)
    handles = struct.pack("<6I", 0x3FF, 0x3FF,
                          uid(-1), uid(-2), 0, off_exit)
    script = sbtmod.Script(count=0, n=2, m=len(code), flag=0,
                           things=b"", handles=handles, code=code, tables=b"")
    return script


def do_selftest():
    tests = []

    def check(name, ok, detail=""):
        tests.append((name, ok, detail))

    row = check_level(script=_synthetic_script(), f=None, docs={},
                      label="synthetic")
    check("counters: things 2/2",
          row["things"]["static_total"] == 2 and row["things"]["vm"] == 2,
          "%s" % row["things"])
    check("counters: door 1/1",
          row["scene"]["doors"]["static"] == 1 and
          row["scene"]["doors"]["vm"] == 1, "%s" % row["scene"]["doors"])
    check("counters: no lights", row["scene"]["lights"]["static"] == 0 and
          row["scene"]["lights"]["vm"] == 0)
    check("exit NEW_LEVEL level2 detected",
          [e for e in row["exits"]["vm"] if e["name"] == "NEW_LEVEL"] and
          [e for e in row["exits"]["vm"]
           if e["target"] == "level2"] != [], "%s" % row["exits"]["vm"])
    check("exit site matches the static one", row["exits_exact"])
    check("scene exact on the synthetic level", row["scene_exact"])
    check("synthetic level is clean", row["ok"], "%s" % row["things"])

    # determinism: two runs, byte-identical JSON
    a = json.dumps(check_level(script=_synthetic_script(), f=None, docs={},
                               label="synthetic"), sort_keys=True)
    b = json.dumps(check_level(script=_synthetic_script(), f=None, docs={},
                               label="synthetic"), sort_keys=True)
    check("determinism: identical JSON", a == b)

    # the checker must *detect* a divergence: a statically described thing
    # behind LEVEL_OBJECTIVES_COMPLETE is unreachable in the synthetic level
    bad = check_level(script=_synthetic_script(extra_thing=True), f=None,
                      docs={}, label="synthetic-bad")
    check("detects an unreachable thing",
          bad["things_exact"] is False and
          bad["things"]["static_total"] == 3 and
          [m["type"] for m in bad["things"].get("missing", [])] == [12],
          "%s" % bad["things"].get("missing"))
    check("detects it via the guard chain",
          bad["things"].get("unreachable")
          and "LEVEL_OBJECTIVES_COMPLETE" in
          " ".join(bad["things"]["unreachable"][0]["conds"]),
          "%s" % bad["things"].get("unreachable"))

    failed = sum(1 for _n, ok, _d in tests if not ok)
    for name, ok, detail in tests:
        print("%s  %s%s" % ("ok  " if ok else "FAIL", name,
                            "" if ok else "  (%s)" % detail))
    print("\n%d tests, %d failed" % (len(tests), failed))
    return 1 if failed else 0


# --------------------------------------------------------------------------
# reporting


def summarise(rows, seconds):
    totals = {
        "levels": len(rows),
        "levels_scene_exact": sum(1 for r in rows if r["scene_exact"]),
        "levels_things_exact": sum(1 for r in rows if r["things_exact"]),
        "levels_exits_exact": sum(1 for r in rows if r["exits_exact"]),
        "levels_ok": sum(1 for r in rows if r["ok"]),
        "levels_docs_ok": sum(1 for r in rows if r["exits"]["docs_ok"]),
        "seconds": round(seconds, 1),
        "objects": collections.Counter(),
        "things": collections.Counter(),
        "exits": collections.Counter(),
        "blocks": sum(r["blocks"] for r in rows),
        "paths": sum(r["paths"] for r in rows),
    }
    for r in rows:
        for cat in SCENE_CATEGORIES:
            totals["objects"][cat + "_static"] += r["scene"][cat]["static"]
            totals["objects"][cat + "_vm"] += r["scene"][cat]["vm"]
        totals["things"]["static_top"] += r["things"]["static_top"]
        totals["things"]["static_recursive"] += r["things"]["static_recursive"]
        totals["things"]["static_conditions"] += \
            r["things"]["static_conditions"]
        totals["things"]["static_total"] += r["things"]["static_total"]
        totals["things"]["vm"] += r["things"]["vm_reachable"]
        totals["things"]["probed"] += r["things"]["vm_probed"]
        totals["things"]["inherited_types"] += r["things"]["inherited_types"]
        totals["exits"]["static"] += len(r["exits"]["static"])
        totals["exits"]["vm"] += len(r["exits"]["vm"])
        totals["exits"]["docs_vs_static"] += len(r["exits"]["docs_vs_static"])
    totals["objects"] = dict(sorted(totals["objects"].items()))
    totals["things"] = dict(sorted(totals["things"].items()))
    totals["exits"] = dict(sorted(totals["exits"].items()))
    return totals


def print_summary(rows, totals, detail=False):
    print("%-8s %6s %8s %7s %7s %5s %4s %4s"
          % ("level", "blocks", "objects", "things", "static", "miss",
             "exit", "docs"))
    for r in rows:
        objs_vm = sum(r["scene"][c]["vm"] for c in SCENE_CATEGORIES)
        objs_st = sum(r["scene"][c]["static"] for c in SCENE_CATEGORIES)
        miss = sum(len(v.get("missing", [])) for v in r["scene"].values())
        miss += len(r["things"].get("unreachable", []))
        print("%-8s %6d %4d/%-3d %7d %7d %5d %4s %4s"
              % (r["level"], r["blocks"], objs_vm, objs_st,
                 r["things"]["vm_reachable"], r["things"]["static_total"],
                 miss,
                 "ok" if r["exits_exact"] else "DIFF",
                 "ok" if r["exits"]["docs_ok"] else "DIFF"))
        if detail:
            for cat, entry in r["scene"].items():
                if entry.get("missing") or entry.get("extra"):
                    print("    %-10s static=%d vm=%d missing=%s extra=%s"
                          " sites=%s" % (cat, entry["static"], entry["vm"],
                                         entry.get("missing", [])[:2],
                                         entry.get("extra", [])[:2],
                                         entry.get("sites", [])[:4]))
            for key in ("extra", "missing", "field_mismatch", "unreachable"):
                if r["things"].get(key):
                    print("    things %-14s %s" % (key, r["things"][key][:2]))
            if r["things"].get("reachable"):
                print("    things reached by the guard-context probe: %d"
                      % len(r["things"]["reachable"]))
            if r["exits"]["static_missing"]:
                print("    exits static-not-executed:", r["exits"][
                    "static_missing"][:3])
            if r["exits"]["vm_extra"]:
                print("    exits vm-not-static:", r["exits"]["vm_extra"][:3])
            if r["exits"]["target_mismatch"]:
                print("    exits target mismatch:",
                      r["exits"]["target_mismatch"][:3])
            if not r["exits"]["docs_ok"]:
                print("    docs vs static:", r["exits"]["docs_vs_static"])
            if not r["docs_name_ok"]:
                print("    docs name %r vs SET_LEVEL_NAME %r"
                      % (r["docs_name"], r["level_name"]))
    print("\n%d levels, %.1fs, %d blocks, %d paths"
          % (totals["levels"], totals["seconds"], totals["blocks"],
             totals["paths"]))
    print("scene  exact: %d/%d   things exact: %d/%d   exits exact: %d/%d"
          "   full: %d/%d"
          % (totals["levels_scene_exact"], totals["levels"],
             totals["levels_things_exact"], totals["levels"],
             totals["levels_exits_exact"], totals["levels"],
             totals["levels_ok"], totals["levels"]))
    print("objects: " + ", ".join(
        "%s %d/%d" % (cat, totals["objects"].get(cat + "_vm", 0),
                      totals["objects"].get(cat + "_static", 0))
        for cat in SCENE_CATEGORIES))
    print("things : vm %(vm)d, static %(static_total)d (recursive"
          " %(static_recursive)d, of which top-level %(static_top)d; plus"
          " %(static_conditions)d used as IF conditions); %(probed)d reached"
          " only with the guard-context probe; type -1 (from source)"
          " %(inherited_types)d" % totals["things"])
    print("exits  : %(vm)d executed, %(static)d static, docs-vs-static"
          " differences %(docs_vs_static)d" % totals["exits"])
    bad = [r for r in rows if not r["ok"]]
    if bad:
        print("levels with a divergence: " + ", ".join(r["level"] for r in bad))
    docbad = [r for r in rows if not r["exits"]["docs_ok"]]
    if docbad:
        print("levels where FORMATS 15 differs from the script: "
              + ", ".join(r["level"] for r in docbad))


def _worker(path):
    return check_level(path)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME",
                                                     DEFAULT_GAME))
    ap.add_argument("--level", help="level stem or path (one level only)")
    ap.add_argument("--json", action="store_true", help="dump the full report")
    ap.add_argument("--summary", action="store_true", help="table (default)")
    ap.add_argument("--detail", action="store_true",
                    help="with --summary: print every divergence")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic script, no game files needed")
    ap.add_argument("--jobs", type=int, default=0,
                    help="worker processes (default: CPU count, max 12)")
    args = ap.parse_args(argv)

    if args.self_test:
        return do_selftest()

    docs = docs_table()
    t0 = time.monotonic()
    if args.level:
        path = args.level
        if not os.path.isfile(path):
            path = os.path.join(args.game, args.level + ".sbt")
        if not os.path.isfile(path):
            print("level not found: %s" % args.level, file=sys.stderr)
            return 1
        rows = [check_level(path, docs=docs)]
    else:
        paths = level_files(args.game)
        if not paths:
            print("no .sbt files in %s" % args.game, file=sys.stderr)
            return 1
        jobs = args.jobs or min(len(paths), os.cpu_count() or 1, 12)
        if jobs > 1 and len(paths) > 1:
            from concurrent.futures import ProcessPoolExecutor
            with ProcessPoolExecutor(max_workers=jobs) as pool:
                rows = list(pool.map(_worker, paths))
        else:
            rows = [check_level(p, docs=docs) for p in paths]
    rows.sort(key=lambda r: r["level"])
    seconds = time.monotonic() - t0
    totals = summarise(rows, seconds)

    if args.json:
        print(json.dumps({"tool": "vm_scene_check", "game": args.game,
                          "contexts": [c.label() for c in CONTEXTS],
                          "levels": rows, "totals": totals}, indent=1,
                         sort_keys=True))
    else:
        print_summary(rows, totals, detail=args.detail)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
