#!/usr/bin/env python3
"""Diff the C++ and Python VM instruction traces (M5.3.2).

Both sides print the same language-neutral dump:

* ``block <i> eid=<id> off=<hex> paths=<n>``
* ``path <j> reason=<name> steps=<n> pc=<hex>``
* ``ev <pc> <depth> <name> <nargs> [<arg>...]``, arg = ``i:<u32>`` |
  ``d:<16 hex>`` (IEEE-754 bits) | ``s:<hex>`` (byte string).

The C++ side is ``engine/tests/vm_trace_dump.cpp``; the Python side is
``gm/sbtvm_exec.Executor`` with ``trace=True``.  Path order inside a block is
implementation detail (frontier order), so paths are compared as multisets.

Usage::

    python3 tools/re/vm_trace_check.py --level level0            # main block
    python3 tools/re/vm_trace_check.py --level dm1 --blocks all
    python3 tools/re/vm_trace_check.py --level level0 --dump-py  # Python only
    python3 tools/re/vm_trace_check.py --self-test               # synthetic
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod          # noqa: E402
from gm import sbtvm_exec as vm       # noqa: E402

# Table instructions keep their payload in `Insn.data`/`SbtTableRecord`, not in
# `args`: Python records a ("n", count) tuple, C++ keeps `args` empty.  The
# tables are compared by the sbtvm goldens, so the trace ignores their args.
# `ELSE` carries ("rel", jump) in Python while C++ keeps the relative target
# in `insn.target`; the jump itself is covered by the golden decodes.
IGNORED_ARG_NAMES = {"DEFINE_WEAPONS", "DEFINE_AMMO", "DEFINE_PLAYER_TYPES",
                     "DEFINE_EQUIPMENT", "DEFINE_LEVEL_OBJECTIVES", "ADD_LIFT",
                     "SOURCEPLAYER_PATH", "ELSE"}

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_DUMP = os.path.join(ROOT, "build", "sandbox", "engine", "tests",
                            "gm_vm_trace_dump")


def resolve_level(value, game):
    if os.path.isfile(value):
        return value
    for cand in (os.path.join(game, value),
                 os.path.join(game, value + ".sbt")):
        if os.path.isfile(cand):
            return cand
    raise SystemExit("level not found: %s" % value)


def load_script(path):
    f = sbtmod.parse_file(path)
    recs = f.records_of(11)
    if not recs:
        raise SystemExit("%s: no SCRIPT record" % path)
    return f, recs[0].payload


def block_offsets(script):
    """Handle order, first occurrence per offset (same walk as the C++ dump)."""
    out = []
    seen = set()
    n = script.n
    if n <= 0 or len(script.handles) < 12 * n:
        return out
    cols = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
    for i in range(n):
        eid = cols[n + i]
        eid = eid - 0x100000000 if eid >= 0x80000000 else eid
        off = cols[2 * n + i]
        if off >= len(script.code) or off in seen:
            continue
        seen.add(off)
        out.append((i, eid, off))
    return out


def format_arg(value):
    if isinstance(value, float):
        return "d:" + struct.pack(">d", value).hex()
    if isinstance(value, str):
        return "s:" + value.encode("latin1").hex()
    return "i:%d" % value


def python_dump(path, picks, limit):
    _f, script = load_script(path)
    state = vm.state_from_script(script)
    ex = vm.Executor(state, max_steps=200000, max_time=0, max_paths=64,
                     on_unknown="both", follow_generate=True,
                     random_unknown=False, trace=True, trace_limit=limit)
    lines = ["# level %s blocks=%d" % (path, len(picks))]
    for index, eid, off in picks:
        paths = ex.run(off, source_entity=eid)
        lines.append("block %d eid=%d off=0x%x paths=%d"
                     % (index, eid, off, len(paths)))
        for j, p in enumerate(paths):
            lines.append("path %d reason=%s steps=%d pc=0x%x"
                         % (j, p.reason, p.steps, p.pc))
            for event in p.trace:
                values = () if event.name in IGNORED_ARG_NAMES else event.args
                args = " ".join(format_arg(a) for a in values)
                lines.append("ev 0x%x %d %s %d%s"
                             % (event.pc, event.depth, event.name,
                                len(values), (" " + args) if args else ""))
    return lines


def cpp_dump(binary, path, picks, limit):
    args = [binary, path, "--limit", str(limit)]
    if picks is not None:
        for index, _eid, _off in picks:
            args += ["--block", str(index)]
    else:
        args += ["--all"]
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit("vm_trace_dump failed: %s" % proc.stderr.strip())
    return proc.stdout.splitlines()


def normalize_event(line):
    """Drop the args of the instructions whose payload is not in `args`."""
    if not line.startswith("ev "):
        return line
    parts = line.split()
    if parts[3] in IGNORED_ARG_NAMES:
        return " ".join(parts[:3] + ["0"])
    return line


def group(lines):
    """Split a dump into {block_index: sorted list of path groups}."""
    blocks = {}
    current = None
    path = None
    for line in lines:
        if line.startswith("#"):
            continue
        if line.startswith("block "):
            fields = dict(part.split("=", 1) for part in line.split()[2:])
            current = {"index": int(line.split()[1]), "head": line,
                       "eid": int(fields["eid"])}
            current["paths"] = []
            blocks[current["index"]] = current
            path = None
        elif line.startswith("path "):
            path = [line]
            current["paths"].append(path)
        elif line.startswith("ev ") and path is not None:
            path.append(normalize_event(line))
        else:
            raise SystemExit("bad dump line: %r" % line)
    for block in blocks.values():
        block["paths"].sort()
    return blocks


def compare(cpp_lines, py_lines, show=5):
    left = group(cpp_lines)
    right = group(py_lines)
    problems = []
    for index in sorted(set(left) | set(right)):
        if index not in left or index not in right:
            problems.append("block %d: only in %s" % (index,
                                                      "C++" if index in left else "Python"))
            continue
        a, b = left[index], right[index]
        if a["eid"] != b["eid"]:
            problems.append("block %d: eid %d != %d" % (index, a["eid"], b["eid"]))
        if len(a["paths"]) != len(b["paths"]):
            problems.append("block %d: paths %d != %d"
                            % (index, len(a["paths"]), len(b["paths"])))
            continue
        for k, (pa, pb) in enumerate(zip(a["paths"], b["paths"])):
            if pa == pb:
                continue
            detail = []
            for x, y in zip(pa, pb):
                if x != y:
                    detail.append("  C++: %s\n  py : %s" % (x, y))
                    break
            if len(pa) != len(pb):
                detail.append("  events %d != %d" % (len(pa) - 1, len(pb) - 1))
            problems.append("block %d path %d differs\n%s"
                            % (index, k, "\n".join(detail[:show])))
    return problems


def self_test():
    sample = ["# level x blocks=1",
              "block 0 eid=7 off=0x0 paths=2",
              "path 0 reason=end steps=3 pc=0x10",
              "ev 0x0 0 IF 0",
              "ev 0x4 0 SET_SOURCEPLAYER_AMMO 2 i:3 d:3ff0000000000000",
              "path 1 reason=end steps=2 pc=0x8",
              "ev 0x0 0 IF 0"]
    checks = 0
    problems = compare(sample, sample)
    assert not problems, problems
    checks += 1
    # Path order must not matter.
    swapped = [sample[0], sample[1], sample[5], sample[6], sample[2], sample[3],
               sample[4]]
    assert not compare(sample, swapped), compare(sample, swapped)
    checks += 1
    # A changed operand is caught.
    bad = list(sample)
    bad[4] = "ev 0x4 0 SET_SOURCEPLAYER_AMMO 2 i:3 d:4000000000000000"
    problems = compare(sample, bad)
    assert problems, "a changed operand was not detected"
    checks += 1
    # A changed pc, a missing event and a missing path are caught.
    assert compare(sample, ["# x", sample[1], sample[2], "ev 0x0 0 IF 0",
                            sample[5], sample[6]])
    checks += 1
    assert compare(sample, sample[:4])
    checks += 1
    assert format_arg(1.0) == "d:3ff0000000000000"
    assert format_arg("Hi") == "s:4869"
    assert format_arg(7) == "i:7"
    checks += 1
    print("selftest: %d/%d" % (checks, checks))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--level", default="level0", help="level name or .sbt path")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--blocks", default="main",
                    help="main | all | comma-separated block indexes")
    ap.add_argument("--limit", type=int, default=20000, help="trace events per path")
    ap.add_argument("--dump-cpp", action="store_true", help="print the C++ dump")
    ap.add_argument("--dump-py", action="store_true", help="print the Python dump")
    ap.add_argument("--binary", default=os.environ.get("GM_VM_TRACE_DUMP",
                                                       DEFAULT_DUMP))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        self_test()
        return 0

    path = resolve_level(args.level, args.game)
    _f, script = load_script(path)
    offsets = block_offsets(script)
    if args.blocks == "main":
        picks = [(0, -1, 0)]
        # The main block is the handle row with offset 0; keep its real id.
        for index, eid, off in offsets:
            if off == 0:
                picks = [(0, eid, 0)]
                break
    elif args.blocks == "all":
        picks = [(i, eid, off) for i, (_, eid, off) in enumerate(offsets)]
    else:
        wanted = {int(x, 0) for x in args.blocks.split(",") if x.strip()}
        picks = [(i, eid, off) for i, (_, eid, off) in enumerate(offsets)
                 if i in wanted]
    if not picks:
        raise SystemExit("%s: no blocks selected" % path)

    py_lines = python_dump(path, picks, args.limit)
    if args.dump_py:
        print("\n".join(py_lines))
    if not os.path.exists(args.binary):
        if not args.dump_py:
            raise SystemExit("missing %s (build gm_vm_trace_dump) or pass "
                             "--dump-py" % args.binary)
        return 0
    cpp_lines = cpp_dump(args.binary, path, picks, args.limit)
    if args.dump_cpp:
        print("\n".join(cpp_lines))

    problems = compare(cpp_lines, py_lines)
    events = sum(1 for line in py_lines if line.startswith("ev "))
    paths = sum(1 for line in py_lines if line.startswith("path "))
    if problems:
        print("trace: %d difference(s) in %s (%d blocks, %d paths, %d events)"
              % (len(problems), os.path.basename(path), len(picks), paths, events))
        for problem in problems[:20]:
            print(problem)
        return 1
    print("ok   %s: %d blocks, %d paths, %d events identical"
          % (os.path.basename(path), len(picks), paths, events))
    return 0


if __name__ == "__main__":
    sys.exit(main())
