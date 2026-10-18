#!/usr/bin/env python3
"""Generate gm/sbtvm.py: embed the SCRIPT VM command tables taken from gundx.exe."""
import struct

EXE = "/mnt/d/games/Gunmetal 3dfx/gundx.exe"
OUT = "/home/nooverload/gunmetal-re/gm/sbtvm.py"

data = open(EXE, "rb").read()


def soff(va):
    if not (0x4A3000 <= va < 0x5B0000):
        return None
    return data[va - 0x401A00:va - 0x401A00 + 120].split(b"\0")[0].decode("latin1")


# region A: {name, sig, f2, size=-1, id} ; region B: {sig, 1, -1, id, name}
COMMANDS = {}
a = 0x4B53E8
while a < 0x4B62E8:
    f = struct.unpack_from("<5I", data, a - 0x401A00)
    COMMANDS[f[4]] = (soff(f[0]), soff(f[1]))
    a += 20

CONDITIONS = {}
a = 0x4B62EC
while a < 0x4B6788:
    f = struct.unpack_from("<5I", data, a - 0x401A00)
    CONDITIONS[f[3]] = (soff(f[4]), soff(f[0]))
    a += 20


def emit_table(name, table):
    lines = [name + " = {"]
    for k in sorted(table):
        nm, sig = table[k]
        lines.append("    %d: (%r, %r)," % (k, nm, sig))
    lines.append("}")
    return "\n".join(lines)


HEADER = '''"""Gunmetal ``.sbt`` SCRIPT (record type 11) virtual machine.

The ``SCRIPT`` record's ``code`` block is a byte stream of *instructions*::

    [u8 opcode][operands]      ...      0xFF = end of sub-script

The opcode is the numeric id of a command.  Two command tables exist in the
shipped ``gundx.exe`` (recovered at ``0x4b53e8`` and ``0x4b62ec``):

* :data:`COMMANDS` -- the 192 *named* world-editor commands (``ADD_DOOR``,
  ``SET_POLY_TEXTURE``, ``LOAD_MUSIC`` ...).  Their *signature* string gives the
  operand layout (one letter per operand).
* :data:`CONDITIONS` -- the 59 commands of the second VM (``ADD_THING``,
  ``FIRE_SOURCEPLAYER_LASER``, ``SOURCEPLAYER_AMMO_AT_LEAST``, ``RANDOM`` ...)
  used by the conditional/expression VM entered through opcode ``0xC9`` and the
  ``IF`` opcode 199.

Operand letters -> size (engine func ``0x457170`` walking the table at
``0x457204``): ``A``/``B``/``D`` = 8 bytes, ``Y`` = 1 byte, ``S`` = a
NUL-terminated string, every other letter = 4 bytes.

A handful of ids have **no entry** in the exe's :data:`COMMANDS` table because
they are *structural* rather than plain commands.  Their *names* and *operand
signatures* are recovered from the engine's text loader (``0x460eca``): every
structural keyword is matched against a pooled string and compiled by emitting
the opcode byte followed by a count and then the record operands walked with
the same signature table (the ``DEFINE_*`` / ``ADD_LIFT`` /
``SOURCEPLAYER_PATH`` / ``IF`` / ``ELSE`` / ``END`` tokens)::

    103  DEFINE_LEVEL_OBJECTIVES  u32 n(<=30) + n strings        ("Objective")
    158  SOURCEPLAYER_PATH        u32 n(<=32) + n * {1 dword}    ("Path points")
    170  DEFINE_WEAPONS           u32 n(<=31) + n * {3 strings + 0x54}
    171  DEFINE_AMMO              u32 n(<=31) + n * {1 string  + 0x28}
    172  DEFINE_PLAYER_TYPES      u32 n        + n * {3 strings + 0x17c}
    175  DEFINE_EQUIPMENT         u32 n(<=31) + n * {1 string  + 0x30}
    188  ADD_LIFT                 0x94 bytes + u32 n(<=31) + n * 0xc ("Lift stops")
    199  IF: u8 cond-id + cond operands + u32 size + body(size)
    200  ELSE: u32 relative offset (jump over the else-branch at run time)
    201  EVAL: u8 cond-id + cond operands (a bare condition statement)

The record sizes above are byte-exact against the pooled signature strings
(``0x4b8fe8`` ``SLEEEEEELLLLL`` = 0x30, ``0x4b900c`` ``SLEOLLLLLLL`` = 0x28,
``0x4b8fa8`` ``SSSLLLLEEEELLLLDBBDL`` = 0x54,
``0x4b9024`` ``SSSLDLLD…EE`` = 0x17c, ``0x4b8f7c`` ``LOLLLLLL…VNVNVN`` =
0x94, ``ELE`` = 0xc, ``E`` = 4).  Opcode ``0xFF`` terminates a sub-script.

``decode()`` turns a code block into a flat list of :class:`Insn` (with ``IF``
bodies decoded recursively) and ``render()`` prints it as readable text.  Every
one of the 36 shipped ``.sbt`` files decodes up to a byte-exact end.
"""

import struct
from collections import namedtuple

'''

LOGIC = r'''

LETTER_SIZE = {
    "A": 8, "B": 8, "D": 8, "Y": 1,
    "E": 4, "G": 4, "I": 4, "L": 4, "M": 4, "N": 4,
    "O": 4, "P": 4, "R": 4, "T": 4, "V": 4, "Z": 4,
    "S": "str",
}

# id -> operand signature of one record for the fixed-size record tables.
# The letters are the very strings the engine's text loader feeds to the
# operand walker (``0x461bc0``), so the derived byte size is authoritative.
TABLE_SIGS = __TABLE_SIGS__
TABLE_NAMES = {
    170: "DEFINE_WEAPONS",
    171: "DEFINE_AMMO",
    172: "DEFINE_PLAYER_TYPES",
    175: "DEFINE_EQUIPMENT",
}
_TABLES = set(TABLE_NAMES.values())

MAX_BLOCK = 0x30000  # engine's code buffer limit (0x4b6784 / [ebp+8] < 0x30000)

Insn = namedtuple("Insn", "off op name args body data")


def _rdstr(code, i):
    j = code.find(b"\0", i)
    if j < 0:
        raise ValueError("unterminated string at 0x%x" % i)
    return code[i:j].decode("latin1"), j + 1


def _letters(sig):
    if not sig:
        return []
    try:
        return [LETTER_SIZE[c] for c in sig]
    except KeyError as exc:
        raise ValueError("bad signature %r (letter %s)" % (sig, exc.args[0]))


def _rdargs(code, i, letters):
    out = []
    for s in letters:
        if s == "str":
            v, i = _rdstr(code, i)
            out.append(v)
        elif s == 8:
            out.append(struct.unpack_from("<d", code, i)[0])
            i += 8
        else:
            out.append(int.from_bytes(code[i:i + s], "little"))
            i += s
    return out, i


def _table(code, i, sig):
    nstr = len(sig) - len(sig.lstrip("S"))
    letters = _letters(sig[nstr:])
    n = struct.unpack_from("<I", code, i)[0]
    i += 4
    recs = []
    for _ in range(n):
        strs = []
        for _ in range(nstr):
            s, i = _rdstr(code, i)
            strs.append(s)
        vals, i = _rdargs(code, i, letters)
        recs.append((strs, vals))
    return recs, i


def decode(code, start=0, end=None):
    """Decode a SCRIPT code block into a list of :class:`Insn`.

    Returns ``(insns, offset_of_end)``; the end offset must equal ``end`` for a
    faithful decode.  ``IF`` bodies are decoded recursively into ``insn.body``.
    """
    if end is None:
        end = len(code)
    out = []
    i = start
    while i < end:
        off = i
        b = code[i]
        if b == 0xFF:                                   # end of sub-script
            out.append(Insn(off, b, "END", [], None, None))
            i += 1
            continue
        if b in TABLE_SIGS:                             # DEFINE_WEAPONS/AMMO/… tables
            recs, i = _table(code, i + 1, TABLE_SIGS[b])
            out.append(Insn(off, b, TABLE_NAMES[b], [("n", len(recs))], None, recs))
            continue
        if b == 103:                                    # DEFINE_LEVEL_OBJECTIVES
            n = struct.unpack_from("<I", code, i + 1)[0]
            if n > 30:
                raise ValueError("DEFINE_LEVEL_OBJECTIVES count too large (%d)" % n)
            i += 5
            vals = []
            for _ in range(n):
                s, i = _rdstr(code, i)
                vals.append(s)
            out.append(Insn(off, b, "DEFINE_LEVEL_OBJECTIVES", vals, None, None))
            continue
        if b == 158:                                    # SOURCEPLAYER_PATH
            n = struct.unpack_from("<I", code, i + 1)[0]
            if n > 32:
                raise ValueError("SOURCEPLAYER_PATH point count too large (%d)" % n)
            i += 5
            vals = list(struct.unpack_from("<%dI" % n, code, i))
            i += 4 * n
            out.append(Insn(off, b, "SOURCEPLAYER_PATH", [("n", n)], None, vals))
            continue
        if b == 188:                                    # ADD_LIFT
            j = i + 1 + 0x94
            head = code[i + 1:j]
            n = struct.unpack_from("<I", code, j)[0]
            j += 4
            recs = [tuple(struct.unpack_from("<3I", code, j + 12 * k)) for k in range(n)]
            i = j + 12 * n
            out.append(Insn(off, b, "ADD_LIFT", [("n", n)], None, (head, recs)))
            continue
        if b == 199:                                    # IF <cond> <size> { body }
            cid = code[i + 1]
            name, sig = CONDITIONS.get(cid, ("OP_%d" % cid, None))
            letters = _letters(sig)
            args, j = _rdargs(code, i + 2, letters)
            size = struct.unpack_from("<I", code, j)[0]
            body, j2 = decode(code, j + 4, j + size)
            out.append(Insn(off, b, "IF", [(name, args)], body, size))
            i = j + size
            # an ELSE clause (opcode 200) always immediately follows the
            # then-branch; decode the else-branch up to the jump target.
            if i < end and code[i] == 200:
                rel = struct.unpack_from("<I", code, i + 1)[0]
                tgt = i + 1 + rel
                ebody, _ = decode(code, i + 5, tgt)
                out.append(Insn(i, 200, "ELSE", [("target", tgt)], ebody, rel))
                i = tgt
            continue
        if b == 200:                                    # ELSE / relative jump
            rel = struct.unpack_from("<I", code, i + 1)[0]
            out.append(Insn(off, b, "ELSE", [("target", i + 1 + rel)], None, rel))
            i = i + 1 + rel
            continue
        if b == 0xC9:                                   # evaluate a condition op
            cid = code[i + 1]
            name, sig = CONDITIONS.get(cid, ("OP_%d" % cid, None))
            args, i = _rdargs(code, i + 2, _letters(sig))
            out.append(Insn(off, b, "EVAL", [(name, args)], None, None))
            continue
        if b not in COMMANDS:
            raise ValueError("unknown opcode %d at 0x%x" % (b, off))
        name, sig = COMMANDS[b]
        args, i = _rdargs(code, i + 1, _letters(sig))
        out.append(Insn(off, b, name, args, None, None))
    return out, i


def _fmt(v):
    if isinstance(v, str):
        return '"%s"' % v
    if isinstance(v, float):
        return "%g" % v
    if isinstance(v, int) and v >= 0x10000:
        return "%d(0x%x)" % (v, v)
    return str(v)


def render(code, start=0, end=None, indent=0):
    """Render a code block as readable text (a mission decompiler)."""
    insns, _ = decode(code, start, end)
    return "\n".join(_render(insns, indent))


def _render(insns, indent):
    """Return the rendered lines as a list of strings."""
    pad = "    " * indent
    lines = []
    k = 0
    n = len(insns)
    while k < n:
        ins = insns[k]
        if ins.name == "END":
            lines.append(pad + "---")
        elif ins.name in _TABLES:
            lines.append("%s%s %d entries" % (pad, ins.name, len(ins.data)))
            for strs, vals in ins.data:
                lines.append("%s    %-24s %s" % (
                    pad, " | ".join(strs), " ".join(_fmt(v) for v in vals)))
        elif ins.name == "DEFINE_LEVEL_OBJECTIVES":
            lines.append(pad + "DEFINE_LEVEL_OBJECTIVES " +
                         " ".join('"%s"' % s for s in ins.args))
        elif ins.name == "SOURCEPLAYER_PATH":
            lines.append("%sSOURCEPLAYER_PATH n=%d %s" % (
                pad, ins.args[0][1], " ".join(_fmt(x) for x in ins.data)))
        elif ins.name == "ADD_LIFT":
            lines.append("%sADD_LIFT n=%d" % (pad, ins.args[0][1]))
        elif ins.name == "IF":
            (cname, cargs), = ins.args
            head = "%sIF %s" % (pad, cname)
            if cargs:
                head += " " + " ".join(_fmt(x) for x in cargs)
            nxt = insns[k + 1] if k + 1 < n else None
            has_else = nxt is not None and nxt.name == "ELSE" and nxt.body is not None
            lines.append(head + " {")
            lines.extend(_render(ins.body, indent + 1))
            if has_else:
                lines.append(pad + "} else {")
                lines.extend(_render(nxt.body, indent + 1))
                k += 1
            lines.append(pad + "}")
        elif ins.name == "ELSE":
            lines.append("%sELSE -> 0x%x" % (pad, ins.args[0][1]))
        elif ins.name == "EVAL":
            (cname, cargs), = ins.args
            lines.append("%sEVAL %s %s" % (pad, cname, " ".join(_fmt(x) for x in cargs)))
        else:
            args = " ".join(_fmt(x) for x in ins.args)
            lines.append(("%s%-34s %s" % (pad, ins.name, args)).rstrip())
        k += 1
    return lines
'''


# signatures + display names of the fixed-size record tables (spec only; the
# generated module gets them through the ``__TABLE_SIGS__`` placeholder).
TABLE_SIGS = {
    170: "SSSLLLLEEEELLLLDBBDL",
    171: "SLEOLLLLLLL",
    172: "SSSLDLLDODDDDLNVNVEEDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDEE",
    175: "SLEEEEEELLLLL",
}
TABLE_NAMES_GEN = {
    170: "DEFINE_WEAPONS",
    171: "DEFINE_AMMO",
    172: "DEFINE_PLAYER_TYPES",
    175: "DEFINE_EQUIPMENT",
}


def main():
    lsize = {"A": 8, "B": 8, "D": 8, "Y": 1, "E": 4, "G": 4, "I": 4, "L": 4,
             "M": 4, "N": 4, "O": 4, "P": 4, "R": 4, "T": 4, "V": 4, "Z": 4}
    expect = {170: 0x54, 171: 0x28, 172: 0x17C, 175: 0x30}
    sig_lit = ["{"]
    for op in sorted(TABLE_SIGS):
        sig = TABLE_SIGS[op]
        got = sum(lsize[c] for c in sig if c != "S")
        assert got == expect[op], "table %d: sig %r gives 0x%x, want 0x%x" % (
            op, sig, got, expect[op])
        sig_lit.append('    %d: "%s",%s' % (op, sig, "  # " + TABLE_NAMES_GEN[op]))
    sig_lit.append("}")
    parts = [HEADER]
    parts.append(emit_table("COMMANDS", COMMANDS))
    parts.append("")
    parts.append(emit_table("CONDITIONS", CONDITIONS))
    parts.append(LOGIC)
    src = "\n".join(parts).replace("__TABLE_SIGS__", "\n".join(sig_lit))
    open(OUT, "w").write(src)
    print("wrote", OUT, "COMMANDS=%d CONDITIONS=%d" % (len(COMMANDS), len(CONDITIONS)))


if __name__ == "__main__":
    main()
