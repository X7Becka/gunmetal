#!/usr/bin/env python3
"""Trace, break, watch and graph Gunmetal ``SCRIPT`` (``.sbt`` record 11) code.

This is the debugging half of the script editor.  It runs the prototype
interpreter :class:`gm.sbtvm_exec.Executor` (no copy of it lives here) and adds
what a debugger needs on top:

* an instruction trace with the *effects* of every step -- events the VM
  emitted and player fields it changed (``gm.sbtvm_exec`` already records both
  on the :class:`~gm.sbtvm_exec.State`, so the trace only has to diff the state
  before/after each instruction);
* breakpoints before an instruction (by code offset or opcode name) and after
  an event (by event name), each followed by a full state dump;
* ``--watch FIELD`` -- only the changes of one VM field (``player.cash``,
  ``vm.ticks`` ...);
* a deterministic control-flow graph of one block as DOT or Mermaid
  (``IF``/``ELSE`` edges, ``GENERATE`` sub-script calls and ``LOOP`` back
  edges), and a block table (``--list-blocks``).

Everything the VM does is :mod:`gm.sbtvm_exec` behaviour; this tool never
interprets byte code on its own.  It only decodes instructions at a byte PC
through ``gm.sbtvm_exec.decode_at``.

Usage::

    python3 tools/editor/sbtdbg.py --run level0 --script main
    python3 tools/editor/sbtdbg.py --run dm1 --script all --trace-limit 200 \\
            --source-player 0 --watch player.cash
    python3 tools/editor/sbtdbg.py --run level0 --script off:0xbd8b \\
            --source-player 0 --break-op ADD_SOURCEPLAYER_CASH --continue 3
    python3 tools/editor/sbtdbg.py --list-blocks level0
    python3 tools/editor/sbtdbg.py --graph level0 --block 0x6f92 \\
            --dot /tmp/opencode/6f92.dot --mermaid /tmp/opencode/6f92.mmd
    python3 tools/editor/sbtdbg.py --self-test

``.sbt`` files live outside the repository; ``--game`` selects the install
(default ``/mnt/d/games/Gunmetal 3dfx``).  The game directory is only read.

Exit codes: ``0`` ran / stopped as asked, ``1`` the VM or the self-test
reported a problem (unknown opcode, unsupported command, step limit ...),
``2`` bad input (no such level, block, breakpoint, watch field, output file).
"""
import argparse
import collections
import difflib
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbt as sbtmod            # noqa: E402
from gm import sbtvm                   # noqa: E402
from gm import sbtvm_exec as vmx       # noqa: E402
from tools.re import vm_exec as vmrun  # noqa: E402
from tools.editor import sbtasm as asmmod  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: ``GENERATE`` family: a call into the row of an entity id
CALL_OPS = {2: "GENERATE", 3: "GENERATE_FOR_CHILDREN", 4: "GENERATE_FOR_PARENT"}

#: commands that end the path (no successor inside the block)
TERMINAL = {
    "EXIT_EVENT", "EXIT_PROGRAM", "NEW_GAME", "NEW_LEVEL", "NEXT_LEVEL",
    "END_GAME",
}

#: reasons after which a block is considered "done" (exit code 0)
CLEAN_REASONS = {
    vmx.END, vmx.RETURN, vmx.PROGRAM_EXIT, vmx.LEVEL_TRANSITION,
    "break-offset", "break-op", "break-event", "step-budget",
}

#: player fields the trace/state/watch expose (the dataclass is authoritative)
PLAYER_FIELDS = (
    "playertype", "cash", "health", "shield", "max_health", "max_shield",
    "key_bits", "weapon_bits", "equipment_bits", "extra_bits", "behaviour",
    "equipment_active", "weapon_index", "equipment_index", "movement_type",
    "salvage", "morale", "secrets",
)

#: VM fields the trace diff/watch expose
VM_FIELDS = ("ticks", "depth", "source_entity", "source_player",
             "objective_bits", "level_name")

SCENE_FIELDS = ("lights", "locations", "items", "named_items", "attached",
                "sounds", "sprites", "things", "doors", "switches", "lifts",
                "spawns")

_WATCH_RE = re.compile(
    r"^(player|vm|scene)\.([A-Za-z_][A-Za-z0-9_]*)(?:\[(-?\d+)\])?$")


class DbgError(Exception):
    """A clean, reportable input error (exit code 2)."""


# ---------------------------------------------------------------------------
# breakpoint specifications


class Break:
    """One ``--break-*`` request: before an instruction or after an event."""

    def __init__(self, kind, value, text):
        self.kind = kind          # offset | op | event
        self.value = value
        self.text = text

    def matches_insn(self, ins):
        if self.kind == "offset":
            return ins.pc == self.value
        if self.kind == "op":
            return ins.name == self.value
        return False

    def matches_event(self, name):
        return self.kind == "event" and name == self.value

    def describe(self):
        return self.text


def _opcode_names():
    """Every name a ``--break-op`` may use (commands + structural + END)."""
    names = {name for name, _sig in sbtvm.COMMANDS.values()}
    names.update(sbtvm.TABLE_NAMES.values())
    names.update(("IF", "ELSE", "EVAL", "END", "DEFINE_LEVEL_OBJECTIVES",
                  "SOURCEPLAYER_PATH", "ADD_LIFT"))
    return names


def _event_names():
    """Every name a ``--break-event`` may use (commands and conditions)."""
    names = {name for name, _sig in sbtvm.COMMANDS.values()}
    names.update(name for name, _sig in sbtvm.CONDITIONS.values())
    names.update(("IF", "ELSE", "EVAL", "END"))
    return names


_OPS = _opcode_names()
_EVENTS = _event_names()


def _hint(name, names):
    """``; did you mean X?`` for a mistyped breakpoint name."""
    close = difflib.get_close_matches(name, sorted(names), n=2)
    return ("; did you mean %s?" % " or ".join(close)) if close else ""


def parse_breaks(offsets=(), ops=(), events=()):
    """Turn the CLI spellings into :class:`Break` objects.

    Accepts ``0x2b07``/``7495`` for ``--break-offset``, a command name for
    ``--break-op`` and a command **or condition** name for ``--break-event``.
    An unknown name is an input error with the closest matches as a hint.
    """
    out = []
    for raw in offsets:
        try:
            value = int(str(raw), 0)
        except ValueError:
            raise DbgError("bad --break-offset %r (want 0x.. or decimal)"
                           % raw)
        if value < 0:
            raise DbgError("--break-offset %r is negative" % raw)
        out.append(Break("offset", value, "offset 0x%x" % value))
    for raw in ops:
        name = str(raw).upper()
        if name not in _OPS:
            raise DbgError("unknown --break-op %r%s (names are the command"
                           " table: GENERATE, ADD_SOURCEPLAYER_CASH, IF ...)"
                           % (raw, _hint(name, _OPS)))
        out.append(Break("op", name, "op %s" % name))
    for raw in events:
        name = str(raw).upper()
        if name not in _EVENTS:
            raise DbgError("unknown --break-event %r%s (events are command"
                           " and condition names)" % (raw, _hint(name, _EVENTS)))
        out.append(Break("event", name, "event %s" % name))
    return out


# ---------------------------------------------------------------------------
# snapshots (what the trace diffs, what --watch reads)


def snapshot(state):
    """The debugger's flat view of one :class:`~gm.sbtvm_exec.State`.

    Only plain values: the trace keeps one per executed instruction, so the
    cost per step is a few dozen scalar copies (an ``ammo`` dict is small: one
    entry per ammo type the script touched).
    """
    index = state.source_player
    p = state.players.get(index) if index is not None else None
    player = None
    if p is not None:
        player = {k: getattr(p, k) for k in PLAYER_FIELDS}
        player["ammo"] = dict(p.ammo)
        player["ammo_max"] = dict(p.ammo_max)
    return {
        "player": player,
        "vm": {
            "pc": state.pc,
            "steps": state.steps,
            "ticks": state.clock,
            "depth": len(state.stack),
            "source_entity": state.source_entity,
            "source_player": state.source_player,
            "objective_bits": state.objectives_bits,
            "level_name": state.level_name,
            "game_mode": state.game_mode,
            "events": len(state.events),
        },
        "scene": state.scene.counts(),
        "nev": len(state.events),
    }


def _fmt(value):
    """One operand, the way ``gm.sbtvm.render`` prints it."""
    if isinstance(value, str):
        return '"%s"' % value
    if isinstance(value, float):
        return "%g" % value
    if isinstance(value, int) and value >= 0x10000:
        return "%d(0x%x)" % (value, value)
    return str(value)


def _args_text(args):
    return " ".join(_fmt(a) for a in args)


def describe(ins):
    """``(name, operands)`` for the trace and graph labels."""
    if ins.kind == "end":
        return "END", ""
    if ins.kind == "if":
        text = ins.cond or "?"
        if ins.cond_args:
            text += " " + _args_text(ins.cond_args)
        return "IF", text
    if ins.kind == "eval":
        text = ins.cond or "?"
        if ins.cond_args:
            text += " " + _args_text(ins.cond_args)
        return "EVAL", text
    if ins.kind == "else":
        return "ELSE", "-> 0x%x" % ins.target
    if ins.kind in ("table", "objectives", "path", "lift"):
        return ins.name, "n=%d" % ins.args[0][1]
    return ins.name, _args_text(ins.args)


def _event_text(name, note):
    """The event name (its operands are already on the trace line)."""
    if note:
        return "event %s (%s)" % (name, note)
    return "event %s" % name


def diff_effects(before, after, events):
    """What one instruction changed: fields, ticks, depth, scene, events."""
    out = []
    pa, pb = before["player"], after["player"]
    if pa is None and pb is not None:
        out.append("player attached")
    elif pa is not None and pb is None:
        out.append("player detached")
    elif pa is not None:
        for key in PLAYER_FIELDS:
            if pa[key] != pb[key]:
                out.append("player.%s %s -> %s"
                           % (key, _fmt(pa[key]), _fmt(pb[key])))
        ammo_a, ammo_b = pa["ammo"], pb["ammo"]
        for atype in sorted(set(ammo_a) | set(ammo_b)):
            va, vb = ammo_a.get(atype), ammo_b.get(atype)
            if va != vb:
                out.append("player.ammo[%d] %s -> %s"
                           % (atype, "-" if va is None else va,
                              "-" if vb is None else vb))
    va, vb = before["vm"], after["vm"]
    for key in VM_FIELDS:
        if va[key] != vb[key]:
            out.append("vm.%s %s -> %s" % (key, _fmt(va[key]), _fmt(vb[key])))
    sa, sb = before["scene"], after["scene"]
    for key in SCENE_FIELDS:
        if sa[key] != sb[key]:
            delta = sb[key] - sa[key]
            out.append("scene.%s %+d (%d -> %d)"
                       % (key, delta, sa[key], sb[key]))
    for _pc, name, _args, note in events:
        out.append(_event_text(name, note))
    return out


# ---------------------------------------------------------------------------
# the executor with breakpoints / a step budget / a trace


class DebugHalt(Exception):
    """Stop requested by the debugger (not a VM error).

    Unlike :class:`gm.sbtvm_exec.Halt` it is **not** caught by
    ``Executor.run``: the live state travels with it, so a stop keeps the
    exact state at the instruction and the driver can resume from it.
    """

    def __init__(self, reason, pc, state, detail=""):
        super().__init__("%s at 0x%x" % (reason, pc))
        self.reason = reason
        self.pc = pc
        self.state = state
        self.detail = detail


class Step:
    """One traced instruction (pre-state) -- the diff base for effects."""

    __slots__ = ("pc", "depth", "name", "args", "kind", "note", "snap")

    def __init__(self, pc, depth, name, args, kind, note, snap):
        self.pc = pc
        self.depth = depth
        self.name = name
        self.args = args
        self.kind = kind
        self.note = note
        self.snap = snap


def dbg_steps(state):
    """The per-state step log (deep-copied together with the state)."""
    steps = state.__dict__.get("dbg_steps")
    if steps is None:
        steps = state.dbg_steps = []
    return steps


class DbgExecutor(vmx.Executor):
    """``gm.sbtvm_exec.Executor`` plus breakpoints and a step budget.

    ``_trace`` is called by the interpreter **before** an instruction runs, so
    a breakpoint there stops with the pre-state and resuming at the same PC
    repeats nothing (the driver disarms the break for exactly one hit).  A
    ``--break-event`` is noticed in ``_event`` and fires at the *next* traced
    instruction, i.e. with the post-event state (and the event handler has
    returned normally).
    """

    def __init__(self, state, *, breaks=(), log_limit=0, budget=None, **kw):
        kw.setdefault("trace", False)
        super().__init__(state, **kw)
        self.breaks = list(breaks)
        self.log_limit = log_limit
        self.budget = budget
        self.skip_breaks_once = False
        self.pending_event = None
        self.executed = 0

    # -- hooks ------------------------------------------------------------
    def _trace(self, st, ins, note=""):
        if note.startswith("fork:"):
            return                      # the interpreter's own fork probe
        if self.pending_event is not None:
            name, pc = self.pending_event
            self.pending_event = None
            raise DebugHalt("break-event", ins.pc, st,
                            "%s fired at 0x%x" % (name, pc))
        skip = self.skip_breaks_once
        self.skip_breaks_once = False
        if not skip:
            for brk in self.breaks:
                if brk.matches_insn(ins):
                    raise DebugHalt("break-" + brk.kind, ins.pc, st,
                                    brk.describe())
        if self.budget is not None:
            if self.budget <= 0:
                raise DebugHalt("step-budget", ins.pc, st, "budget spent")
            self.budget -= 1
        self.executed += 1
        if self.log_limit > 0:
            steps = dbg_steps(st)
            # one extra step beyond the limit: the snapshot of the next
            # instruction is the post-state the last printed step needs
            if len(steps) <= self.log_limit:
                name, args = describe(ins)
                steps.append(Step(ins.pc, len(st.stack), name, args,
                                  ins.kind, note, snapshot(st)))

    def _event(self, st, ins, name=None, note=""):
        super()._event(st, ins, name, note)
        ev = name or ins.name
        for brk in self.breaks:
            if brk.matches_event(ev):
                self.pending_event = (ev, ins.pc)
                break


# ---------------------------------------------------------------------------
# level / block helpers


def load_level(level, game):
    """Resolve a level and return ``(path, script)``; all failures -> DbgError."""
    try:
        path = vmrun.resolve_level(level, game)
        _sbt, script = vmrun.load_script(path)
    except SystemExit as exc:
        raise DbgError(str(exc) or "cannot read %s" % level)
    except (OSError, ValueError, struct.error) as exc:
        raise DbgError("%s: %s" % (level, exc))
    return path, script


def block_spans(script):
    """``[(start, end)]`` per sub-script, from ``tools/editor/sbtasm``."""
    try:
        return asmmod.block_spans(script.code, asmmod.block_starts(script))
    except (ValueError, struct.error) as exc:
        raise DbgError("cannot split the script into blocks: %s" % exc)


def block_at(spans, offset):
    for start, end in spans:
        if start == offset:
            return start, end
    raise DbgError("no block starts at 0x%x (%d blocks; try --list-blocks)"
                   % (offset, len(spans)))


def walk_block(code, start, end):
    """Yield the instructions of one block in byte order (bodies descended).

    ``decode_at().next`` skips an ``IF`` body, so the walk keeps a stack of
    body ends, exactly like ``tools/editor/sbtasm.census``: every byte of the
    block is visited once, including code that physically follows an ``ELSE``.
    """
    stack = []
    pos = start
    while pos < end:
        while stack and pos >= stack[-1]:
            stack.pop()
        ins = vmx.decode_at(code, pos)
        yield ins
        if ins.kind == "end":
            return
        if ins.kind == "if":
            stack.append(ins.body_end)
            pos = ins.body_start
            continue
        if ins.kind == "else":
            pos += 5
            continue
        pos = ins.next


def entity_offsets(script):
    """``id -> offset`` of the first entity row with a code offset."""
    out = {}
    try:
        st = vmx.state_from_script(script)
    except (ValueError, struct.error):
        return out
    for eid, rows in st.entity_rows.items():
        for row in rows:
            if row.offset is not None:
                out.setdefault(eid, row.offset)
                break
    return out


def block_census(code, start, end):
    """Per-block counters used by ``--list-blocks`` and the graph."""
    ops = collections.Counter()
    for ins in walk_block(code, start, end):
        if ins.kind in ("table", "objectives", "path", "lift"):
            ops["DEFINE"] += 1
        else:
            ops[ins.kind] += 1
        ops["total"] += 1
    return ops


# ---------------------------------------------------------------------------
# control-flow graph


def _label(ins):
    name, args = describe(ins)
    text = "0x%06x %s" % (ins.pc, name)
    if args:
        text += " " + args
    if len(text) > 90:
        text = text[:87] + "..."
    return text


def build_graph(code, start, end, ent_offsets=None):
    """Deterministic CFG of one block.

    Nodes: one per instruction, plus one per distinct ``GENERATE`` target
    (``kind == "call"``) and one ``exit`` node for an edge that leaves the
    block.  Edges: ``true``/``false`` from an ``IF``, ``jump`` from an
    ``ELSE``, ``next`` for the fall-through, ``call`` into a sub-script.
    Instruction nodes are ordered by PC, call nodes by target offset, so the
    text of the DOT/Mermaid output is stable across runs.
    """
    ent_offsets = ent_offsets or {}
    nodes = []
    by_pc = {}
    for ins in walk_block(code, start, end):
        node = {"id": "n%06x" % ins.pc, "pc": ins.pc,
                "kind": {"if": "if", "else": "else", "eval": "eval",
                         "end": "end", "cmd": "cmd"}.get(ins.kind, "cmd"),
                "label": _label(ins)}
        nodes.append(node)
        by_pc[ins.pc] = node

    calls = {}
    exits = {}
    edges = []

    def node_for(addr):
        node = by_pc.get(addr)
        if node is not None:
            return node
        node = exits.get(addr)
        if node is None:
            node = {"id": "x%06x" % addr, "pc": addr, "kind": "exit",
                    "label": "outside block 0x%06x" % addr}
            exits[addr] = node
            nodes.append(node)
        return node

    def call_node(target, eid):
        key = target if target is not None else ("id", eid)
        node = calls.get(key)
        if node is None:
            if target is None:
                node = {"id": "c_e%d" % eid, "pc": -1, "kind": "call",
                        "label": "call entity %d (no row)" % eid}
            else:
                node = {"id": "c%06x" % target, "pc": target, "kind": "call",
                        "label": "call 0x%06x (entity %d)"
                                 % (target, eid)}
            calls[key] = node
            nodes.append(node)
        return node

    def edge(src, dst, label, kind):
        item = {"src": src["id"], "dst": dst["id"], "label": label,
                "kind": kind}
        if item not in edges:
            edges.append(item)

    for ins in walk_block(code, start, end):
        src = by_pc[ins.pc]
        if ins.kind == "end":
            continue
        if ins.kind == "if":
            edge(src, node_for(ins.body_start), "true", "true")
            edge(src, node_for(ins.body_end), "false", "false")
            continue
        if ins.kind == "else":
            edge(src, node_for(ins.target), "jump", "jump")
            continue
        if ins.name in TERMINAL:
            continue
        edge(src, node_for(ins.next), "next", "next")
        if ins.op in CALL_OPS and ins.args:
            eid = vmx.s32(ins.args[0])
            target = ent_offsets.get(eid)
            edge(src, call_node(target, eid), "call", "call")

    order = {node["id"]: k for k, node in enumerate(nodes)}
    edges.sort(key=lambda e: (order[e["src"]], e["label"], order[e["dst"]]))
    loops = _loop_edges(nodes, edges, start, end)
    for item in loops:
        # mark the closing edge itself; a self-recursive GENERATE is then
        # visible as "loop" in both text formats, not only in the counter
        item["kind"] = "loop"
        item["label"] = "loop"
        item["loop"] = True
    stats = collections.Counter(node["kind"] for node in nodes)
    stats["nodes"] = len(nodes)
    stats["edges"] = len(edges)
    stats["loops"] = len(loops)
    return {"nodes": nodes, "edges": edges, "loops": loops,
            "stats": dict(sorted(stats.items()))}


def _loop_edges(nodes, edges, start, end):
    """Edges that close a cycle: a back edge or a self-recursive call.

    An ``ELSE`` is an unsigned relative offset, so a block can never jump
    backwards; loops in the shipped scripts are made by ``GENERATE``: a row
    whose offset lies inside the same block re-enters it (``dm1`` has 6 such
    blocks, ``level0`` 7).  Both cases are reported as a LOOP edge.
    """
    pc = {node["id"]: node["pc"] for node in nodes}
    out = []
    for item in edges:
        src, dst = item["src"], item["dst"]
        if item["kind"] in ("true", "false", "jump", "next"):
            if pc[dst] <= pc[src]:
                out.append(item)
        elif item["kind"] == "call" and start <= pc[dst] < end:
            out.append(item)
    return out


def _dot_escape(text):
    return (text.replace("\\", "\\\\").replace('"', '\\"')
            .replace("\n", "\\n"))


def to_dot(graph, title):
    lines = ['digraph script {',
             '  graph [rankdir=TB, labelloc=t, label="%s"];'
             % _dot_escape(title),
             '  node [shape=box, fontname="monospace", fontsize=10];',
             '  edge [fontname="monospace", fontsize=9];']
    shapes = {"if": "diamond", "else": "hexagon", "call": "box",
              "exit": "oval", "end": "circle", "eval": "ellipse",
              "cmd": "box"}
    for node in graph["nodes"]:
        attrs = ['label="%s"' % _dot_escape(node["label"]),
                 "shape=%s" % shapes.get(node["kind"], "box")]
        if node["kind"] == "call":
            attrs.append("style=dashed")
        lines.append("  %s [%s];" % (node["id"], ", ".join(attrs)))
    for item in graph["edges"]:
        style = (", style=dashed, color=red" if item["kind"] == "loop"
                 else ", style=dashed" if item["kind"] == "call" else "")
        lines.append('  %s -> %s [label="%s"%s];'
                     % (item["src"], item["dst"], item["label"], style))
    lines.append("}")
    return "\n".join(lines) + "\n"


def _mm_escape(text):
    for src, dst in (("&", "&amp;"), ('"', "#quot;"), ("<", "#lt;"),
                     (">", "#gt;"), ("|", "#124;"), ("\n", "<br/>")):
        text = text.replace(src, dst)
    return text


def to_mermaid(graph, title):
    lines = ["flowchart TD",
             "    %%%% %s" % title.replace("\n", " ")]
    shapes = {
        "if": '{"%s"}',
        "else": '{{"%s"}}',
        "call": '[["%s"]]',
        "exit": '(["%s"])',
        "end": '(["%s"])',
        "eval": '(["%s"])',
        "cmd": '["%s"]',
    }
    for node in graph["nodes"]:
        fmt = shapes.get(node["kind"], '["%s"]')
        lines.append("    %s%s" % (node["id"], fmt % _mm_escape(node["label"])))
    for item in graph["edges"]:
        arrow = "-.->" if item["kind"] in ("call", "loop") else "-->"
        lines.append("    %s %s|%s| %s"
                     % (item["src"], arrow, item["label"], item["dst"]))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# trace / watch / state reporting


def _fmt_value(value):
    if isinstance(value, dict):
        if not value:
            return "{}"
        return "{" + ", ".join("%s: %s" % (k, v)
                               for k, v in sorted(value.items())) + "}"
    if isinstance(value, float):
        return "%g" % value
    return str(value)


def state_dict(state):
    """The state of a stop, as plain JSON-able data."""
    snap = snapshot(state)
    events = collections.Counter(name for _pc, name, _a, _n in state.events)
    snap["events_total"] = len(state.events)
    snap["event_counts"] = dict(sorted(events.items()))
    snap["scene_counts"] = snap.pop("scene")
    snap["spawns"] = [list(s.pos) for s in state.scene.spawns[:1]]
    return snap


def state_lines(state, title):
    """Human-readable dump of one stop (player, VM counters, events, scene)."""
    snap = state_dict(state)
    vm = snap["vm"]
    out = [title,
           "  vm: pc=0x%x steps=%d ticks=%d depth=%d events=%d"
           " source_entity=%s source_player=%s"
           % (vm["pc"], vm["steps"], vm["ticks"], vm["depth"],
              vm["events"], vm["source_entity"], vm["source_player"])]
    player = snap["player"]
    if player is None:
        out.append("  player: no source player (SOURCE_CLEAR / level script)")
    else:
        out.append("  player[%s]: cash=%s health=%s shield=%s"
                   " ammo=%s weapon_bits=0x%x key_bits=0x%x"
                   % (vm["source_player"], _fmt_value(player["cash"]),
                      _fmt_value(player["health"]), _fmt_value(player["shield"]),
                      _fmt_value(player["ammo"]),
                      player["weapon_bits"], player["key_bits"]))
        out.append("      playertype=%d movement_type=%d equipment=0x%x"
                   " extra=0x%x behaviour=0x%x salvage=%d morale=%d secrets=%d"
                   % (player["playertype"], player["movement_type"],
                      player["equipment_bits"], player["extra_bits"],
                      player["behaviour"], player["salvage"],
                      player["morale"], player["secrets"]))
        out.append("      weapon_index=%d equipment_index=%d"
                   " equipment_active=0x%x"
                   % (player["weapon_index"], player["equipment_index"],
                      player["equipment_active"]))
        out.append("  player.pos: not modelled by gm.sbtvm_exec"
                   " (no player pose); scene.spawns=%d%s"
                   % (len(snap["spawns"]),
                      " first=%s" % (tuple(snap["spawns"][0]),)
                      if snap["spawns"] else ""))
    counts = snap["event_counts"]
    text = ", ".join("%s=%d" % kv for kv in counts.items()) or "none"
    out.append("  events (%d): %s" % (snap["events_total"], text))
    scene = ", ".join("%s=%d" % kv for kv in snap["scene_counts"].items()
                      if kv[1]) or "empty"
    out.append("  scene: %s" % scene)
    return out


def trace_rows(steps, final_state, limit=0):
    """Trace rows with the effects of every step.

    ``steps[i].snap`` is the state *before* step ``i``, so the effect of step
    ``i`` is the diff to ``steps[i+1]`` (or to the final state for the last
    step), and the events of step ``i`` are ``state.events[snap_i.nev:...]``.
    """
    rows = []
    final_snap = snapshot(final_state)
    events = final_state.events
    for k, step in enumerate(steps):
        if limit and k >= limit:
            break
        after = steps[k + 1].snap if k + 1 < len(steps) else final_snap
        new_events = events[step.snap["nev"]:after["nev"]]
        effects = diff_effects(step.snap, after, new_events)
        rows.append({
            "pc": step.pc,
            "depth": step.depth,
            "op": step.name,
            "args": step.args,
            "effects": effects,
        })
    return rows


def watch_rows(steps, final_state, field, limit=0):
    """Only the changes of *field*, attributed to the instruction that did it."""
    if not steps:
        return []
    final_snap = snapshot(final_state)
    rows = []
    values = [s.snap for s in steps] + [final_snap]
    for k, step in enumerate(steps[:limit or None]):
        before = values[k]
        after = values[k + 1]
        va = resolve_field(before, field)
        vb = resolve_field(after, field)
        if va != vb:
            rows.append({"pc": step.pc, "op": step.name,
                         "from": va, "to": vb})
    return rows


def validate_watch(field):
    """Reject an unknown ``--watch`` field with the list of valid ones."""
    m = _WATCH_RE.match(field)
    if not m:
        raise DbgError("bad --watch %r (use player.cash, vm.ticks,"
                       " player.ammo[3], scene.items); valid: %s"
                       % (field, ", ".join(watch_fields())))
    group, key, index = m.group(1), m.group(2), m.group(3)
    if group == "scene":
        if key not in SCENE_FIELDS:
            raise DbgError("unknown --watch %r; scene fields: %s"
                           % (field, ", ".join(SCENE_FIELDS)))
    elif group == "vm":
        if key not in VM_FIELDS + ("steps", "pc", "events", "game_mode",
                                   "clock"):
            raise DbgError("unknown --watch %r; vm fields: %s"
                           % (field, ", ".join(VM_FIELDS + (
                               "steps", "pc", "events", "game_mode"))))
    else:
        if key not in PLAYER_FIELDS + ("ammo", "ammo_max"):
            raise DbgError("unknown --watch %r; player fields: %s"
                           % (field, ", ".join(PLAYER_FIELDS
                                               + ("ammo", "ammo_max"))))
        if index is not None and key != "ammo":
            raise DbgError("--watch %r: an index is only supported on"
                           " player.ammo" % field)


def resolve_field(snap, field):
    """Value of a ``--watch`` field in a snapshot (``None`` if absent)."""
    m = _WATCH_RE.match(field)
    if not m:
        raise DbgError("bad --watch %r" % field)
    group, key, index = m.group(1), m.group(2), m.group(3)
    if group == "scene":
        return snap["scene"].get(key)
    if group == "vm":
        if key == "clock":
            key = "ticks"
        return snap["vm"].get(key)
    player = snap["player"]
    if player is None:
        return None
    if index is not None:
        return player[key].get(int(index))
    return player.get(key)


def watch_fields():
    names = ["player." + k for k in PLAYER_FIELDS]
    names += ["player.ammo", "player.ammo[N]", "player.ammo_max"]
    names += ["vm." + k for k in VM_FIELDS]
    names += ["vm.steps", "vm.pc", "vm.events", "vm.game_mode", "vm.clock"]
    names += ["scene." + k for k in SCENE_FIELDS]
    return names


# ---------------------------------------------------------------------------
# running with stops


class Stop:
    __slots__ = ("reason", "pc", "detail", "state")

    def __init__(self, reason, pc, detail, state):
        self.reason = reason
        self.pc = pc
        self.detail = detail
        self.state = state


def run_with_stops(ex, start, source_entity, cont):
    """Run *start*, honouring breakpoints; return ``(results, stops, halted)``.

    ``cont`` is ``None`` (stop at the first break) or the number of
    instructions to execute *after* the first stop.  A resume re-enters the
    interpreter at the stop PC with the stop's state; the break that just
    fired is disarmed for exactly one hit so nothing runs twice.
    """
    stops = []
    first = True
    resume = start
    budget = None
    safety = 0
    while True:
        safety += 1
        if safety > 1000:
            raise DbgError("too many stops (%d); raise --continue or narrow"
                           " the breakpoints" % len(stops))
        ex.budget = budget
        try:
            if first:
                results = ex.run(resume, source_entity=source_entity)
            else:
                results = ex.run(resume)
        except DebugHalt as halt:
            stops.append(Stop(halt.reason, halt.pc, halt.detail, halt.state))
            ex.st = halt.state
            resume = halt.pc
            if halt.reason in ("break-offset", "break-op"):
                ex.skip_breaks_once = True
            if cont is None:
                return [], stops, True
            if budget is None:
                budget = cont
            # the hook already spent the instructions that ran in this segment
            budget = ex.budget if ex.budget is not None else budget
            if budget <= 0:
                return [], stops, True
            first = False
            continue
        # the run returned normally: an event break may still be pending
        state = results[0].state if results else ex.st
        if ex.pending_event is not None:
            name, pc = ex.pending_event
            ex.pending_event = None
            stops.append(Stop("break-event", state.pc,
                              "%s fired at 0x%x (end of path)" % (name, pc),
                              state))
        return results, stops, False


# ---------------------------------------------------------------------------
# CLI commands


def _new_executor(script, args, breaks):
    """``vm_exec.make_executor`` gives the state and the context knobs."""
    base = vmrun.make_executor(script, args)
    return DbgExecutor(base.st, max_steps=base.max_steps,
                       max_time=base.max_time, max_paths=base.max_paths,
                       on_unknown=base.on_unknown,
                       follow_generate=base.follow,
                       random_unknown=base.random_unknown,
                       trace=False, trace_limit=0,
                       breaks=breaks, log_limit=args.trace_limit)


def _fmt_script_tag(eid, off):
    if eid is not None:
        return "entity %s" % eid
    return "main" if off == 0 else "block"


def cmd_run(args):
    path, script = load_level(args.run, args.game)
    if args.watch and args.trace_limit <= 0:
        raise DbgError("--watch needs --trace-limit > 0 (got %d)"
                       % args.trace_limit)
    breaks = parse_breaks(args.break_offset, args.break_op, args.break_event)
    try:
        picks = vmrun.select_scripts(script, args.script)
    except SystemExit as exc:
        raise DbgError(str(exc))
    if args.script != "all" and not picks:
        raise DbgError("--script %r selected no block" % args.script)
    for field in args.watch:
        validate_watch(field)

    blocks = []
    failed = []
    for index, (eid, off) in enumerate(picks):
        ex = _new_executor(script, args, breaks)
        results, stops, _halted = run_with_stops(ex, off, eid, args.cont)
        if results:
            state = results[0].state
            reason = results[0].reason
            paths = len(results)
        else:
            state = stops[-1].state if stops else ex.st
            reason = stops[-1].reason if stops else "halted"
            paths = 0
        steps = dbg_steps(state)
        steps_this = len(steps)
        rows = trace_rows(steps, state, args.trace_limit)
        watch = {field: watch_rows(steps, state, field, args.trace_limit)
                 for field in args.watch}
        notes = list(ex.notes)
        if reason not in CLEAN_REASONS:
            failed.append((off, reason))
        if breaks and not stops:
            notes.append("breakpoint(s) not hit: %s"
                         % ", ".join(b.text for b in breaks))
        entry = {
            "script": _fmt_script_tag(eid, off),
            "off": "0x%x" % off,
            "reason": reason,
            "paths": paths,
            "steps": ex.executed,
            "trace_steps": steps_this,
            "notes": notes,
            "trace": rows,
            "watch": watch,
            "stops": [{"reason": s.reason, "pc": "0x%x" % s.pc,
                       "detail": s.detail, "state": state_dict(s.state)}
                      for s in stops],
            "state": state_dict(state),
            "scene": state.scene.counts(),
        }
        blocks.append(entry)
        if not args.json:
            _print_block(entry, args, script.code, index, stops,
                         os.path.basename(path), show=index < args.show)

    if args.json:
        print(json.dumps({"level": os.path.basename(path),
                          "game": args.game,
                          "blocks": blocks}, indent=1))
    else:
        _print_run_footer(blocks, failed)
    return 1 if failed else 0


def _print_block(entry, args, code, index, stops, level_name, show=True):
    print("%s %s %s (off %s)"
          % ("===" if index == 0 else "---", level_name, entry["script"],
             entry["off"]))
    print("  reason=%s steps=%d paths=%d scene=%s"
          % (entry["reason"], entry["steps"], entry["paths"],
             ", ".join("%s=%d" % kv for kv in entry["scene"].items()
                       if kv[1]) or "empty"))
    if not show:
        _print_watch(entry)
        return
    for k, stop in enumerate(stops):
        ins = None
        try:
            ins = vmx.decode_at(code, stop.pc)
        except vmx.VMError:
            ins = None
        name, operands = describe(ins) if ins is not None else ("?", "")
        print()
        lines = state_lines(stop.state, "stop %d: %s (%s)"
                            % (k + 1, stop.reason, stop.detail))
        for line in lines:
            print(line)
        print("  at 0x%x  %s %s" % (stop.pc, name, operands))
    if entry["notes"]:
        print("  notes:")
        for note in entry["notes"][:8]:
            print("    - " + note)
    if entry["trace"] and (args.trace or not args.watch):
        rows = entry["trace"]
        print("  trace (%d of %d executed instructions):"
              % (len(rows), entry["steps"]))
        for row in rows:
            eff = "; ".join(row["effects"])
            print("    %08x %s%-28s %s%s"
                  % (row["pc"], "  " * row["depth"], row["op"], row["args"],
                     ("  ; " + eff) if eff else ""))
    for field, rows in entry["watch"].items():
        if rows:
            print("  watch %s: %d change(s)" % (field, len(rows)))
            for row in rows:
                print("    %08x  %-28s %s -> %s"
                      % (row["pc"], row["op"], _fmt_value(row["from"]),
                         _fmt_value(row["to"])))
        else:
            print("  watch %s: no changes in %d traced step(s)"
                  % (field, len(entry["trace"])))


def _print_watch(entry):
    for field, rows in entry["watch"].items():
        if rows:
            print("  watch %s: %d change(s)" % (field, len(rows)))
            for row in rows:
                print("    %08x  %-28s %s -> %s"
                      % (row["pc"], row["op"], _fmt_value(row["from"]),
                         _fmt_value(row["to"])))


def _print_run_footer(blocks, failed):
    total = sum(b["steps"] for b in blocks)
    reasons = collections.Counter(b["reason"] for b in blocks)
    print("\n%d block(s), %d steps, stop reasons: %s"
          % (len(blocks), total,
             ", ".join("%s=%d" % kv for kv in sorted(reasons.items()))))
    if failed:
        print("%d block(s) stopped on a diagnostic: %s"
              % (len(failed), ", ".join("0x%x %s" % kv for kv in failed)))




def cmd_list_blocks(args):
    path, script = load_level(args.list_blocks, args.game)
    spans = block_spans(script)
    code = script.code
    ents = entity_offsets(script)
    rows = []
    totals = collections.Counter()
    for start, end in spans:
        census = block_census(code, start, end)
        graph = build_graph(code, start, end, ents)
        loops = graph["stats"].get("loops", 0)
        row = {"off": "0x%x" % start, "size": end - start,
               "ops": census.get("total", 0), "if": census.get("if", 0),
               "else": census.get("else", 0), "eval": census.get("eval", 0),
               "define": census.get("DEFINE", 0),
               "calls": graph["stats"].get("call", 0), "loops": loops}
        rows.append(row)
        totals.update({"blocks": 1, "ops": row["ops"], "if": row["if"],
                       "else": row["else"], "eval": row["eval"],
                       "define": row["define"], "calls": row["calls"],
                       "loops": loops, "size": row["size"]})
    if args.json:
        print(json.dumps({"level": os.path.basename(path),
                          "blocks": rows, "totals": dict(totals)}, indent=1))
        return 0
    print("%-10s %8s %6s %5s %5s %5s %6s %6s %5s"
          % ("block", "size", "ops", "IF", "ELSE", "EVAL", "DEFINE",
             "calls", "LOOP"))
    for row in rows:
        print("%-10s %8d %6d %5d %5d %5d %6d %6d %5d"
              % (row["off"], row["size"], row["ops"], row["if"],
                 row["else"], row["eval"], row["define"], row["calls"],
                 row["loops"]))
    print("%-10s %8d %6d %5d %5d %5d %6d %6d %5d"
          % ("total", totals["size"], totals["ops"], totals["if"],
             totals["else"], totals["eval"], totals["define"],
             totals["calls"], totals["loops"]))
    print("%d block(s), %d byte(s)" % (totals["blocks"], totals["size"]))
    return 0


def cmd_graph(args):
    path, script = load_level(args.graph, args.game)
    if not args.dot and not args.mermaid:
        raise DbgError("--graph needs --dot FILE and/or --mermaid FILE")
    try:
        offset = int(str(args.block), 0)
    except ValueError:
        raise DbgError("bad --block %r (want 0x.. or decimal)" % args.block)
    spans = block_spans(script)
    start, end = block_at(spans, offset)
    graph = build_graph(script.code, start, end, entity_offsets(script))
    title = "%s block 0x%x (0x%x..0x%x)" % (
        os.path.basename(path), start, start, end)
    written = []
    text_dot = text_mm = None
    if args.dot:
        text_dot = to_dot(graph, title)
        try:
            with open(args.dot, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text_dot)
        except OSError as exc:
            raise DbgError("cannot write %s: %s" % (args.dot, exc))
        written.append(args.dot)
    if args.mermaid:
        text_mm = to_mermaid(graph, title)
        try:
            with open(args.mermaid, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text_mm)
        except OSError as exc:
            raise DbgError("cannot write %s: %s" % (args.mermaid, exc))
        written.append(args.mermaid)
    stats = graph["stats"]
    if args.json:
        kinds = {k: stats[k] for k in ("if", "else", "eval", "cmd", "call",
                                       "exit", "end") if stats.get(k)}
        print(json.dumps({"level": os.path.basename(path),
                          "block": "0x%x" % start, "size": end - start,
                          "nodes": stats["nodes"], "edges": stats["edges"],
                          "kinds": kinds, "loops": stats.get("loops", 0),
                          "files": written}, indent=1))
    else:
        print("%s block 0x%x: %d bytes, %d nodes (%s), %d edges, %d loop(s)"
              % (os.path.basename(path), start, end - start, stats["nodes"],
                 ", ".join("%s=%d" % (k, stats[k]) for k in
                           ("if", "else", "eval", "cmd", "call", "exit",
                            "end") if stats.get(k)),
                 stats["edges"], stats.get("loops", 0)))
        for target in written:
            print("wrote %s" % target)
    return 0


# ---------------------------------------------------------------------------
# self-test (no game files)


def _synthetic_script():
    """A SCRIPT record built in memory and round-tripped through gm.sbt.

    ``gm.sbt.encode_payload(11, Script(...))`` is the same writer the file
    editor uses; parsing it back proves the debugger sees an ordinary record.
    The code is assembled from the interpreter's own synthetic encoder
    (``gm.sbtvm_exec._enc_cmd``/``enc_if``), so no game file is involved.
    """
    enc = vmx._enc_cmd
    cash_then = enc("ADD_SOURCEPLAYER_CASH", 100)
    cash_else = enc("ADD_SOURCEPLAYER_CASH", 250)
    body = (vmx.enc_if(6, [2], cash_then, cash_else)      # difficulty >= 2?
            + enc("SLEEP", 7) + enc("GENERATE", 7) + b"\xff")
    sub = enc("LOG_MESSAGE", "sub ran") + enc("ADD_SOURCEPLAYER_AMMO", 11, 3)
    sub += b"\xff"
    code = body + sub
    handles = struct.pack("<3I", 0x3FF, 7, len(body))
    script = sbtmod.Script(count=0, n=1, m=len(code), flag=0, things=b"",
                           handles=handles, code=code, tables=b"")
    payload = sbtmod.encode_payload(11, script)
    parsed = sbtmod.parse_payload(11, payload)
    return script, parsed, payload, len(body)


def _selftest_checks():
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), "" if cond else str(detail)))

    script, parsed, payload, sub_off = _synthetic_script()
    check("synthetic SCRIPT: encode_payload(11) -> parse_payload round-trip",
          sbtmod.encode_payload(11, parsed) == payload
          and parsed.code == script.code and parsed.n == 1,
          "payload %d bytes" % len(payload))
    check("synthetic SCRIPT: blocks 0x0 and 0x%x" % sub_off,
          block_starts_of(script) == [0, sub_off], block_starts_of(script))

    # -- breakpoint specification parser
    brks = parse_breaks(["0x10", "16"], ["GENERATE"], ["LOG_MESSAGE"])
    check("break specs: offset/op/event parsed",
          [(b.kind, b.value) for b in brks]
          == [("offset", 16), ("offset", 16), ("op", "GENERATE"),
              ("event", "LOG_MESSAGE")], brks)
    bad = 0
    for kwargs in ({"offsets": ["zzz"]}, {"ops": ["NOPE"]},
                   {"events": ["NOPE"]}):
        try:
            parse_breaks(**kwargs)
        except DbgError:
            bad += 1
    check("break specs: bad offset/op/event rejected", bad == 3, bad)

    # -- plain trace on the synthetic script
    state = vmx.state_from_script(parsed, source_player=0, difficulty=1)
    ex = DbgExecutor(state, breaks=[], log_limit=100, trace=False,
                     on_unknown="false")
    results = ex.run(0, source_entity=None)
    steps = dbg_steps(results[0].state)
    names = [s.name for s in steps]
    check("trace: IF -> else branch (difficulty 1)", results[0].reason == vmx.END
          and names[:4] == ["IF", "ADD_SOURCEPLAYER_CASH", "SLEEP",
                            "GENERATE"], names)
    rows = trace_rows(steps, results[0].state)
    cash = [r for r in rows if r["op"] == "ADD_SOURCEPLAYER_CASH"][0]
    check("trace: effects show player.cash 0 -> 250",
          "player.cash 0 -> 250" in cash["effects"], cash["effects"])
    check("trace: GENERATE enters the sub-script (depth 1)",
          [r["depth"] for r in rows if r["op"] == "LOG_MESSAGE"] == [1],
          [(r["op"], r["depth"]) for r in rows])
    log = [r for r in rows if r["op"] == "LOG_MESSAGE"][0]
    check("trace: event LOG_MESSAGE in the effects",
          any("event LOG_MESSAGE" in e for e in log["effects"]), log["effects"])

    # -- breakpoint before the instruction
    state = vmx.state_from_script(parsed, source_player=0, difficulty=1)
    target = [s.pc for s in steps if s.name == "ADD_SOURCEPLAYER_CASH"][0]
    ex = DbgExecutor(state, breaks=parse_breaks(["0x%x" % target]),
                     log_limit=100, trace=False, on_unknown="false")
    results, stops, halted = run_with_stops(ex, 0, None, None)
    check("break-offset: stops exactly there, before it runs",
          halted and len(stops) == 1 and stops[0].pc == target
          and stops[0].state.players[0].cash == 0,
          (len(stops), stops[0].pc if stops else None))
    # ... and --continue 1 executes it (250), then stops on the budget
    state = vmx.state_from_script(parsed, source_player=0, difficulty=1)
    ex = DbgExecutor(state, breaks=parse_breaks(["0x%x" % target]),
                     log_limit=100, trace=False, on_unknown="false")
    results, stops, halted = run_with_stops(ex, 0, None, 1)
    check("break-offset + --continue 1: instruction runs once",
          len(stops) == 2 and stops[0].reason == "break-offset"
          and stops[0].state.players[0].cash == 0
          and stops[1].reason == "step-budget"
          and stops[1].state.players[0].cash == 250,
          [(s.reason, s.state.players[0].cash) for s in stops])

    # -- breakpoint on an opcode / on an event
    state = vmx.state_from_script(parsed, source_player=0, difficulty=1)
    ex = DbgExecutor(state, breaks=parse_breaks([], ["GENERATE"]),
                     log_limit=100, trace=False, on_unknown="false")
    results, stops, halted = run_with_stops(ex, 0, None, None)
    check("break-op: stops before GENERATE",
          halted and len(stops) == 1 and stops[0].reason == "break-op"
          and stops[0].state.players[0].cash == 250,
          (len(stops), stops[0].reason if stops else None))
    state = vmx.state_from_script(parsed, source_player=0, difficulty=1)
    ex = DbgExecutor(state, breaks=parse_breaks([], [], ["LOG_MESSAGE"]),
                     log_limit=100, trace=False, on_unknown="false")
    results, stops, halted = run_with_stops(ex, 0, None, None)
    stop = stops[0] if stops else None
    check("break-event: stops after the event, before the next instruction",
          stop is not None
          and any(e[1] == "LOG_MESSAGE" for e in stop.state.events)
          and len(stop.state.stack) == 1
          and stop.pc == [s.pc for s in steps
                          if s.name == "ADD_SOURCEPLAYER_AMMO"][0],
          (stop.detail, stop.pc) if stop else None)

    # -- step budget (after a stop)
    state = vmx.state_from_script(parsed, source_player=0, difficulty=1)
    ex = DbgExecutor(state, breaks=parse_breaks(["0x0"]), log_limit=100,
                     trace=False, on_unknown="false")
    results, stops, halted = run_with_stops(ex, 0, None, 2)
    check("step budget: --continue 2 executes two instructions",
          halted and len(stops) == 2 and stops[1].reason == "step-budget"
          and len(dbg_steps(stops[1].state)) == 2
          and stops[1].state.players[0].cash == 250,
          [(s.reason, len(dbg_steps(s.state))) for s in stops])

    # -- watch
    rows = watch_rows(steps, results[0].state if results else ex.st,
                      "player.cash")
    check("watch: only changes of player.cash", rows
          and rows[0]["from"] == 0 and rows[0]["to"] == 250
          and len(rows) == 1, rows)
    rejected = 0
    for bad in ("player.nope", "vm.nope", "scene.nope", "nope",
                "player.cash[0]"):
        try:
            validate_watch(bad)
        except DbgError:
            rejected += 1
    check("watch: unknown fields rejected", rejected == 5, rejected)

    # -- graph
    spans = block_spans(parsed)
    graph = build_graph(parsed.code, spans[0][0], spans[0][1],
                        entity_offsets(parsed))
    kinds = graph["stats"]
    labels = [n["label"] for n in graph["nodes"]]
    check("graph: IF, ELSE, call and END nodes present",
          kinds.get("if") == 1 and kinds.get("else") == 1
          and kinds.get("call") == 1 and kinds.get("end") == 1, kinds)
    check("graph: call node resolves the entity offset",
          any(n["kind"] == "call" and n["pc"] == sub_off
              for n in graph["nodes"]), [n for n in graph["nodes"]
                                         if n["kind"] == "call"])
    check("graph: every edge endpoint is a node",
          {e["src"] for e in graph["edges"]} <= {n["id"] for n in graph["nodes"]}
          and {e["dst"] for e in graph["edges"]}
          <= {n["id"] for n in graph["nodes"]}, graph["edges"])
    check("graph: forward-only CFG has no loops", kinds.get("loops") == 0,
          graph["loops"])
    fake = [{"src": "b", "dst": "a", "kind": "next", "label": "next"},
            {"src": "a", "dst": "b", "kind": "jump", "label": "jump"}]
    fake_nodes = [{"id": "a", "pc": 0}, {"id": "b", "pc": 5}]
    check("graph: a back edge is counted as a LOOP",
          _loop_edges(fake_nodes, fake, 0, 10) == [fake[0]],
          _loop_edges(fake_nodes, fake, 0, 10))
    dot = to_dot(graph, "test block")
    mm = to_mermaid(graph, "test block")
    check("graph: DOT text is a digraph with all nodes/edges",
          dot.startswith("digraph script {") and dot.rstrip().endswith("}")
          and sum(1 for line in dot.splitlines()
                  if re.match(r"^\s+\w+ -> \w+ \[", line))
          == len(graph["edges"])
          and sum(1 for n in graph["nodes"] if "  %s [" % n["id"] in dot)
          == len(graph["nodes"]), (dot.count(" -> "),
                                   len(graph["edges"])))
    check("graph: Mermaid text is flowchart TD with all nodes/edges",
          mm.startswith("flowchart TD\n") and mm.count("-->")
          + mm.count("-.->") == len(graph["edges"])
          and all(n["id"] in mm for n in graph["nodes"]),
          (mm.count("-->"), len(graph["edges"])))
    check("graph: node order is stable across two builds",
          [n["id"] for n in build_graph(parsed.code, spans[0][0], spans[0][1],
                                        entity_offsets(parsed))["nodes"]]
          == [n["id"] for n in graph["nodes"]])

    # -- a self-recursive GENERATE is a LOOP edge, not just a counter
    gen = vmx._enc_cmd("GENERATE", 7) + b"\xff"
    sub_handles = struct.pack("<3I", 0x3FF, 7, 0)
    self_script = sbtmod.Script(count=0, n=1, m=len(gen), flag=0, things=b"",
                                handles=sub_handles, code=gen, tables=b"")
    self_parsed = sbtmod.parse_payload(
        11, sbtmod.encode_payload(11, self_script))
    self_graph = build_graph(self_parsed.code, 0, len(gen),
                             entity_offsets(self_parsed))
    loop_edges = [e for e in self_graph["edges"] if e["kind"] == "loop"]
    check("graph: self-recursive GENERATE gives a LOOP edge",
          self_graph["stats"]["loops"] == 1 and len(loop_edges) == 1
          and loop_edges[0]["label"] == "loop"
          and self_graph["stats"]["call"] == 1, self_graph["edges"])

    # -- block table
    rows = []
    for start, end in spans:
        census = block_census(parsed.code, start, end)
        rows.append((start, end - start, census.get("total", 0),
                     census.get("if", 0), census.get("else", 0)))
    check("list-blocks: two blocks with their census", rows == [
        (0, sub_off, 7, 1, 1),
        (sub_off, len(parsed.code) - sub_off, 3, 0, 0)], rows)

    # -- stop dump is complete
    text = "\n".join(state_lines(results[0].state if results else ex.st, "s"))
    check("state dump: vm/player/events/scene present",
          "vm: pc=" in text and "player[0]:" in text and "events (" in text
          and "scene:" in text, text)
    return checks


def block_starts_of(script):
    return sorted(set(asmmod.block_starts(script)))


def do_selftest(json_out=False):
    checks = _selftest_checks()
    failed = [c for c in checks if not c[1]]
    if json_out:
        print(json.dumps({"tests": [{"name": n, "ok": ok, "detail": d}
                                    for n, ok, d in checks],
                          "passed": len(checks) - len(failed),
                          "total": len(checks)}, indent=1))
    else:
        for name, ok, detail in checks:
            print("%s  %s%s" % ("ok  " if ok else "FAIL", name,
                                "" if ok else "  (%s)" % detail))
        print("\n%d/%d self-tests passed"
              % (len(checks) - len(failed), len(checks)))
    return not failed


# ---------------------------------------------------------------------------
# main


def build_parser():
    ap = argparse.ArgumentParser(
        description="Trace, break, watch and graph .sbt SCRIPT sub-scripts.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    mode = ap.add_argument_group("mode (one of)")
    mode.add_argument("--run", metavar="LEVEL",
                      help="run a level's blocks and print the trace")
    mode.add_argument("--list-blocks", metavar="LEVEL",
                      help="table of sub-scripts (offset, size, ops, IF/LOOP)")
    mode.add_argument("--graph", metavar="LEVEL",
                      help="control-flow graph of --block as DOT/Mermaid")
    mode.add_argument("--self-test", action="store_true",
                      help="synthetic tests (no game files)")

    run = ap.add_argument_group("run")
    run.add_argument("--script", default="main",
                     help="main | all | <index> | id:<id> | off:<hex>")
    run.add_argument("--break-offset", action="append", default=[],
                     metavar="0x..", help="stop before the instruction here")
    run.add_argument("--break-op", action="append", default=[],
                     metavar="NAME", help="stop before this opcode")
    run.add_argument("--break-event", action="append", default=[],
                     metavar="NAME", help="stop after this event")
    run.add_argument("--continue", dest="cont", type=int, default=None,
                     metavar="N",
                     help="after a stop, execute up to N more instructions")
    run.add_argument("--watch", action="append", default=[], metavar="FIELD",
                     help="print only changes of FIELD (player.cash, vm.ticks,"
                          " player.ammo[3], scene.items)")
    run.add_argument("--trace", action="store_true",
                     help="with --watch: also print the full trace")
    run.add_argument("--trace-limit", type=int, default=50,
                     help="traced instructions per block (0 = none)")
    run.add_argument("--show", type=int, default=8,
                     help="with --script all: blocks printed in full")
    run.add_argument("--max-steps", type=int, default=200_000)
    run.add_argument("--max-time", type=float, default=5.0,
                     help="wall-clock budget per block (0 = none)")
    run.add_argument("--max-paths", type=int, default=64)
    run.add_argument("--on-unknown", choices=("stop", "false", "true", "both"),
                     default="false",
                     help="a condition the model cannot prove: assume false "
                          "(default, one path), stop, true, or explore both")
    run.add_argument("--random-unknown", action="store_true",
                     help="treat RANDOM as unknown")
    run.add_argument("--follow", action="store_true",
                     help="execute GENERATE sub-scripts (default for one block)")
    run.add_argument("--no-follow", action="store_true",
                     help="do not execute GENERATE sub-scripts")
    run.add_argument("--game-mode", choices=sorted(vmrun.GAME_MODES),
                     default="campaign")
    run.add_argument("--players", type=int, default=1)
    run.add_argument("--difficulty", type=int, default=1)
    run.add_argument("--level-number", type=int, default=0)
    run.add_argument("--god-mode", action="store_true")
    run.add_argument("--source-player", type=int, default=None,
                     help="make the source a player (0..7): player fields and"
                          " SOURCEPLAYER_* conditions then have a value")
    run.add_argument("--seed", type=int, default=0x12345678)

    graph = ap.add_argument_group("graph/list")
    graph.add_argument("--block", default="0x0",
                       help="block offset for --graph (default 0x0 = main)")
    graph.add_argument("--dot", metavar="FILE", help="write Graphviz DOT here")
    graph.add_argument("--mermaid", metavar="FILE",
                       help="write a Mermaid flowchart here")

    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--json", action="store_true")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.no_follow:
        args.follow = False
    try:
        if args.self_test:
            return 0 if do_selftest(args.json) else 1
        if args.graph:
            return cmd_graph(args)
        if args.list_blocks:
            return cmd_list_blocks(args)
        if args.run:
            return cmd_run(args)
    except DbgError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    build_parser().print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except DbgError as exc:
        print("error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, struct.error, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        sys.exit(2)
