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

A handful of ids have **no name** in the exe's table because they are
*structural* rather than text commands; their layouts come from their handlers
(see :data:`SPECIAL`)::

    103  u32 n (<=30) + n strings
    158  u32 n + n u32
    170  u32 n(<=c8) + n * {3 strings + 0x54 bytes}   weapons table
    171  u32 n + n * {1 string  + 0x28 bytes}          items table
    172  u32 n(<=c8) + n * {3 strings + 0x17c bytes}   chassis/thing table
    175  u32 n + n * {1 string  + 0x30 bytes}          equipment table
    188  0x94 bytes + u32 n(<=c8) + n * 0xc bytes
    199  IF: u8 cond + cond operands + u32 size + body(size)
    200  GOTO: u32 relative offset

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

# id -> (n_strings, n_bytes) fixed-size record tables
SIMPLE_TABLES = {
    170: (3, 0x54),
    171: (1, 0x28),
    175: (1, 0x30),
}
TABLE_NAMES = {
    170: "WEAPONS",
    171: "ITEMS",
    175: "EQUIPMENT",
}

# id -> (n_strings, n_bytes) for the "thing/chassis" table (172)
THING_TABLE = (3, 0x17C)

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


def _table(code, i, nstr, nbytes):
    n = struct.unpack_from("<I", code, i)[0]
    i += 4
    recs = []
    for _ in range(n):
        strs = []
        for _ in range(nstr):
            s, i = _rdstr(code, i)
            strs.append(s)
        vals = list(struct.unpack_from("<%dI" % (nbytes // 4), code, i))
        i += nbytes
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
        if b in SIMPLE_TABLES:                          # items/weapons/equipment
            recs, i = _table(code, i + 1, *SIMPLE_TABLES[b])
            out.append(Insn(off, b, TABLE_NAMES[b], [("n", len(recs))], None, recs))
            continue
        if b == 172:                                    # chassis / thing table
            recs, i = _table(code, i + 1, *THING_TABLE)
            out.append(Insn(off, b, "THINGS", [("n", len(recs))], None, recs))
            continue
        if b == 103:                                    # <=30 strings
            n = struct.unpack_from("<I", code, i + 1)[0]
            if n > 30:
                raise ValueError("OP103 count too large (%d)" % n)
            i += 5
            vals = []
            for _ in range(n):
                s, i = _rdstr(code, i)
                vals.append(s)
            out.append(Insn(off, b, "STRINGS", vals, None, None))
            continue
        if b == 158:                                    # n u32
            n = struct.unpack_from("<I", code, i + 1)[0]
            i += 5
            vals = list(struct.unpack_from("<%dI" % n, code, i))
            i += 4 * n
            out.append(Insn(off, b, "INTS", vals, None, None))
            continue
        if b == 188:                                    # 0x94 + n*0xc
            j = i + 1 + 0x94
            head = code[i + 1:j]
            n = struct.unpack_from("<I", code, j)[0]
            j += 4
            recs = [tuple(struct.unpack_from("<3I", code, j + 12 * k)) for k in range(n)]
            i = j + 12 * n
            out.append(Insn(off, b, "OP188", [("n", n)], None, (head, recs)))
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
            continue
        if b == 200:                                    # GOTO <relative>
            rel = struct.unpack_from("<I", code, i + 1)[0]
            out.append(Insn(off, b, "GOTO", [("target", i + 1 + rel)], None, rel))
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
    for ins in insns:
        if ins.name == "END":
            lines.append(pad + "---")
        elif ins.name in TABLE_NAMES.values() or ins.name == "THINGS":
            lines.append("%s%s %d entries" % (pad, ins.name, len(ins.data)))
            for strs, vals in ins.data:
                lines.append("%s    %-24s %s" % (pad, " | ".join(strs), vals))
        elif ins.name == "STRINGS":
            lines.append(pad + "STRINGS " + " ".join('"%s"' % s for s in ins.args))
        elif ins.name == "INTS":
            lines.append(pad + "INTS " + " ".join(_fmt(x) for x in ins.args))
        elif ins.name == "OP188":
            lines.append("%sOP188 n=%d" % (pad, ins.args[0][1]))
        elif ins.name == "IF":
            (cname, cargs), = ins.args
            head = "%sIF %s" % (pad, cname)
            if cargs:
                head += " " + " ".join(_fmt(x) for x in cargs)
            lines.append(head + " {")
            lines.extend(_render(ins.body, indent + 1))
            lines.append(pad + "}")
        elif ins.name == "GOTO":
            lines.append("%sGOTO -> 0x%x" % (pad, ins.args[0][1]))
        elif ins.name == "EVAL":
            (cname, cargs), = ins.args
            lines.append("%sEVAL %s %s" % (pad, cname, " ".join(_fmt(x) for x in cargs)))
        else:
            args = " ".join(_fmt(x) for x in ins.args)
            lines.append(("%s%-34s %s" % (pad, ins.name, args)).rstrip())
    return lines
'''


def main():
    parts = [HEADER]
    parts.append(emit_table("COMMANDS", COMMANDS))
    parts.append("")
    parts.append(emit_table("CONDITIONS", CONDITIONS))
    parts.append(LOGIC)
    open(OUT, "w").write("\n".join(parts))
    print("wrote", OUT, "COMMANDS=%d CONDITIONS=%d" % (len(COMMANDS), len(CONDITIONS)))


if __name__ == "__main__":
    main()
