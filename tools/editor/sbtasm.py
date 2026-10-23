#!/usr/bin/env python3
"""Assembler for Gunmetal ``SCRIPT`` (``.sbt`` record 11) sub-script bytecode.

This is the inverse of :func:`gm.sbtvm.decode`: it turns the **text** back into
the byte stream, so a block can be extracted, edited and re-assembled (writing
the result into a ``.sbt`` is *not* part of this tool -- see ``M11.4``).

The text dialect is exactly the output of :func:`gm.sbtvm.render` (the reference
decompiler), extended in the places where that renderer is lossy:

* doubles are printed with ``%g`` (6 significant digits) by ``render``; the
  assembler accepts the exact shortest round-trip form (``repr``) as well, and
  the local :func:`render_exact` emits it.  A raw 64-bit pattern
  ``0x<16 hex digits>`` is also accepted for a double operand.
* ``ADD_LIFT`` -- ``render`` prints only ``n``; :func:`render_exact` appends
  ``head=<hex of 0x94 bytes>`` and one line per stop.  Assembling reference
  text without ``head=`` is an error unless ``allow_lossy`` is set (the header
  is then zero-filled, which is only useful for measuring the loss).
* ``ELSE`` whose else-code lies outside the enclosing ``IF``'s byte range:
  ``render`` nests it inside the outer braces (so its text cannot be
  re-assembled), :func:`render_exact` emits the flat ``ELSE -> 0x...`` form.

Everything else is byte-preserving through ``render``: integers (including the
``12345(0x3039)`` annotation of large values), quoted strings, table records,
``IF``/``ELSE`` sizes and targets, ``EVAL``, ``DEFINE_LEVEL_OBJECTIVES``,
``SOURCEPLAYER_PATH``.  ``DB``/``RAW`` are escape hatches for raw bytes.

Grammar (block-local; offsets are relative to the first byte of the text)::

    line       := label | insn | if | else | table | db | comment | blank
    label      := '.' NAME ':'
    insn       := NAME [operand...]              ; COMMANDS / structural
    if         := 'IF' COND [operand...] '{' line* '}'
                  [ 'else' '{' line* '}' ]       ; '}' and 'else' on one line
    else       := 'ELSE' '->' (NUMBER | label)
    table      := NAME N 'entries' record*
    record     := STRING (' | ' STRING)* value*
    db         := 'DB' (NUMBER | STRING)+ | 'RAW' HEXSTRING
    comment    := ';' ... | '#' ... | '//' ...
    operand    := NUMBER | NUMBER '(0x' HEX ')' | FLOAT | '0x' 16HEX | STRING
    END        := '---'                          ; opcode 0xFF

Operand letters and their byte sizes are taken from :mod:`gm.sbtvm`
(``A``/``B``/``D`` = ``double``, ``Y`` = ``u8``, ``S`` = NUL-terminated string,
everything else = ``u32``).

Usage::

    python3 tools/editor/sbtasm.py disasm dm1.sbt --block 0x2b07
    python3 tools/editor/sbtasm.py disasm dm1.sbt --block all --out /var/tmp/dm1
    python3 tools/editor/sbtasm.py asm /var/tmp/dm1/00002b07.asm -o /var/tmp/b.bin
    python3 tools/editor/sbtasm.py roundtrip              # 36 levels, exact
    python3 tools/editor/sbtasm.py roundtrip --render both --json
    python3 tools/editor/sbtasm.py self-test              # no game files needed

Text errors never escape as tracebacks: every failure is an :class:`AsmError`
carrying the 1-based text line number.
"""
import argparse
import collections
import glob
import json
import math
import os
import re
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbt as sbtmod            # noqa: E402
from gm import sbtvm                   # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: structural opcodes: id -> name (none of them is in ``sbtvm.COMMANDS``)
STRUCT_OPS = {
    103: "DEFINE_LEVEL_OBJECTIVES",
    158: "SOURCEPLAYER_PATH",
    188: "ADD_LIFT",
    199: "IF",
    200: "ELSE",
    201: "EVAL",
}

_LETTER_SIZE = sbtvm.LETTER_SIZE

#: plain named commands (table opcodes are handled as tables, not as commands)
CMD_BY_NAME = {}
for _op, (_name, _sig) in sbtvm.COMMANDS.items():
    if _op in sbtvm.TABLE_SIGS:
        continue
    CMD_BY_NAME[_name] = (_op, _sig or "")

COND_BY_NAME = {_name: (_cid, _sig or "")
                for _cid, (_name, _sig) in sbtvm.CONDITIONS.items()}

_TABLE_BY_NAME = {name: op for op, name in sbtvm.TABLE_NAMES.items()}

_INT_RE = re.compile(r"^(-?(?:0[xX][0-9a-fA-F]+|\d+))(?:\(0[xX][0-9a-fA-F]+\))?$")
_DBL_BITS_RE = re.compile(r"^0[xX]([0-9a-fA-F]{16})$")
_OP_COND_RE = re.compile(r"^OP_(\d+)$")
_LABEL_RE = re.compile(r"^([A-Za-z_.][\w.]*):$")
_TABLE_HDR_RE = re.compile(r"^(\w+)\s+(\d+)\s+entries\s*$")
_PATH_RE = re.compile(r"^SOURCEPLAYER_PATH\s+n=(\d+)\s*(.*)$")
_LIFT_RE = re.compile(r"^ADD_LIFT\s+n=(\d+)\s*(.*)$")


class AsmError(Exception):
    """A text-level assembly error, carrying the 1-based source line."""

    def __init__(self, line, message):
        self.line = line
        self.message = message
        super().__init__(message)

    def __str__(self):
        if self.line:
            return "line %d: %s" % (self.line, self.message)
        return self.message


# ---------------------------------------------------------------------------
# lexical helpers


def _strip_comment(line):
    """Remove ``;``/``#``/``//`` comments, respecting quoted strings."""
    out = []
    i = 0
    n = len(line)
    quoted = False
    while i < n:
        ch = line[i]
        if quoted:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if ch == '"':
                quoted = False
            i += 1
            continue
        if ch == '"':
            quoted = True
            out.append(ch)
            i += 1
            continue
        if ch == ";" or ch == "#":
            break
        if ch == "/" and i + 1 < n and line[i + 1] == "/":
            break
        out.append(ch)
        i += 1
    return "".join(out)


def _scan_tokens(text, line):
    """Split into ``('raw', text)`` / ``('str', '"..."')`` tokens.

    Quoted strings stay one token even when they contain spaces.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        if text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 2
                    continue
                j += 1
            if j >= n:
                raise AsmError(line, "unterminated string literal")
            out.append(("str", text[i:j + 1]))
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace():
                j += 1
            out.append(("raw", text[i:j]))
            i = j
    return out


def _unquote(token, line):
    """Turn a ``"..."`` literal into a latin-1 string."""
    text = token
    if not (text.startswith('"') and text.endswith('"') and len(text) >= 2):
        raise AsmError(line, "expected a quoted string, got %r" % text)
    out = []
    i = 1
    n = len(text) - 1
    simple = {"n": "\n", "r": "\r", "t": "\t", "0": "\0",
              "\\": "\\", '"': '"'}
    while i < n:
        ch = text[i]
        if ch == "\\":
            if i + 1 >= n:
                raise AsmError(line, "trailing backslash in string")
            esc = text[i + 1]
            if esc == "x":
                hexpart = text[i + 2:i + 4]
                if not re.match(r"^[0-9a-fA-F]{2}$", hexpart):
                    raise AsmError(line, "bad \\x escape in string")
                out.append(chr(int(hexpart, 16)))
                i += 4
                continue
            if esc not in simple:
                raise AsmError(line, "unknown escape \\%s in string" % esc)
            out.append(simple[esc])
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _string_bytes(value, line):
    try:
        raw = value.encode("latin1")
    except UnicodeEncodeError:
        raise AsmError(line, "string %r is not latin-1" % value)
    if b"\0" in raw:
        raise AsmError(line, "NUL byte inside a string literal")
    return raw + b"\0"


def _parse_int(token, bits, line):
    m = _INT_RE.match(token)
    if not m:
        raise AsmError(line, "expected an integer, got %r" % token)
    try:
        value = int(m.group(1), 0)
    except ValueError:
        raise AsmError(line, "bad integer literal %r" % token)
    mask = (1 << bits) - 1
    if value < 0:
        value &= mask
    elif value > mask:
        raise AsmError(line, "value %s does not fit in %d bits" % (token, bits))
    return value


def _parse_double(token, line):
    m = _DBL_BITS_RE.match(token)
    if m:
        return struct.unpack("<d", bytes.fromhex(m.group(1)))[0]
    try:
        value = float(token)
    except ValueError:
        raise AsmError(line, "expected a number, got %r" % token)
    if not math.isfinite(value):
        raise AsmError(line, "%r is not finite; use the 0x<16 hex> bit form" % token)
    return value


def _operand(token, letter, line):
    kind, text = token
    if letter == "S":
        if kind != "str":
            raise AsmError(line, "expected a quoted string for an S operand")
        return _unquote(text, line)
    if kind != "raw":
        raise AsmError(line, "string literal where a number was expected")
    if letter in "ABD":
        return _parse_double(text, line)
    return _parse_int(text, 8 * _LETTER_SIZE[letter], line)


def _operands(tokens, sig, line, what="command"):
    if len(tokens) != len(sig):
        raise AsmError(
            line, "%s expects %d operand(s) [%s], got %d"
            % (what, len(sig), sig or "-", len(tokens)))
    return [_operand(tok, letter, line) for tok, letter in zip(tokens, sig)]


def _split_pipe(head, line):
    """Split a table record head on ``|`` outside of quoted strings.

    ``gm.sbtvm.render`` separates the strings with ``" | "`` and pads the
    joined text, so the leading separator of an empty first string loses its
    leading space (``" | b"`` becomes ``"| b"`` after stripping).  Splitting on
    a bare ``|`` handles both; strings that really contain ``|`` are quoted by
    :func:`render_exact` and therefore still unambiguous.
    """
    parts = []
    start = 0
    i = 0
    n = len(head)
    quoted = False
    while i < n:
        ch = head[i]
        if quoted:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                quoted = False
            i += 1
            continue
        if ch == '"':
            quoted = True
            i += 1
            continue
        if ch == "|":
            parts.append(head[start:i])
            i += 1
            start = i
            continue
        i += 1
    parts.append(head[start:])
    return parts


def _table_string(part, line):
    part = part.strip()
    if part.startswith('"'):
        return _unquote(part, line)
    return part


# ---------------------------------------------------------------------------
# parsed program


class Node(object):
    """One parsed line (or IF with its nested bodies)."""

    def __init__(self, kind, line, **fields):
        self.kind = kind
        self.line = line
        self.__dict__.update(fields)


class Parser(object):
    def __init__(self, text, allow_lossy=False):
        if text.startswith("\ufeff"):            # tolerate a UTF-8 BOM
            text = text[1:]
        self.lines = text.splitlines()
        self.allow_lossy = allow_lossy
        self.i = 0

    # -- helpers ---------------------------------------------------------
    def _text(self, index):
        return _strip_comment(self.lines[index]).strip()

    def _fail(self, line, message):
        raise AsmError(line, message)

    def parse(self):
        nodes = self._seq(top=True)
        if self.i < len(self.lines):
            text = self._text(self.i)
            if text:
                raise AsmError(self.i + 1, "unexpected %r" % text)
            self.i += 1
        return nodes

    def _ends_seq(self, text):
        return text == "}" or text == "} else {"

    # -- grammar ---------------------------------------------------------
    def _seq(self, top):
        nodes = []
        while self.i < len(self.lines):
            text = self._text(self.i)
            if not text:
                self.i += 1
                continue
            if self._ends_seq(text):
                if top:
                    raise AsmError(self.i + 1, "unexpected %r" % text)
                return nodes
            line = self.i + 1
            m = _LABEL_RE.match(text)
            if m:
                nodes.append(Node("label", line, name=m.group(1)))
                self.i += 1
                continue
            word = text.split()[0]
            upper = word.upper()
            if text == "---":
                nodes.append(Node("end", line))
                self.i += 1
            elif upper == "IF":
                nodes.append(self._parse_if(text, line))
            elif upper == "ELSE":
                nodes.append(self._parse_else(text, line))
                self.i += 1
            elif upper == "EVAL":
                nodes.append(self._parse_eval(text, line))
                self.i += 1
            elif upper in _TABLE_BY_NAME and _TABLE_HDR_RE.match(text):
                nodes.append(self._parse_table(text, line, upper))
            elif upper == "DEFINE_LEVEL_OBJECTIVES":
                nodes.append(self._parse_objectives(text, line))
                self.i += 1
            elif upper == "SOURCEPLAYER_PATH":
                nodes.append(self._parse_path(text, line))
                self.i += 1
            elif upper == "ADD_LIFT":
                nodes.append(self._parse_lift(text, line))
            elif upper == "DB":
                nodes.append(self._parse_db(text, line))
                self.i += 1
            elif upper == "RAW":
                nodes.append(self._parse_raw(text, line))
                self.i += 1
            elif upper in CMD_BY_NAME:
                op, sig = CMD_BY_NAME[upper]
                toks = _scan_tokens(text, line)[1:]
                args = _operands(toks, sig, line, what=upper)
                nodes.append(Node("insn", line, name=upper, op=op,
                                  args=args, sig=sig))
                self.i += 1
            else:
                self._fail(
                    line,
                    "unknown command %r (use DB/RAW for raw bytes)" % word)
        return nodes

    def _parse_if(self, text, line):
        toks = _scan_tokens(text, line)
        if len(toks) < 3 or toks[-1] != ("raw", "{"):
            self._fail(line, "IF must end with '{'")
        name = toks[1][1]
        cid, sig = self._cond(name, line)
        args = _operands(toks[2:-1], sig, line, what="condition " + name)
        self.i += 1
        body = self._seq(top=False)
        if self.i >= len(self.lines):
            self._fail(line, "IF without '}'")
        term = self._text(self.i)
        else_body = None
        if term == "}":
            self.i += 1
        elif term == "} else {":
            self.i += 1
            else_body = self._seq(top=False)
            if self.i >= len(self.lines) or self._text(self.i) != "}":
                self._fail(line, "IF/else without closing '}'")
            self.i += 1
        else:
            self._fail(self.i + 1, "expected '}' or '} else {', got %r" % term)
        return Node("if", line, cid=cid, name=name, args=args,
                    body=body, else_body=else_body)

    def _parse_else(self, text, line):
        toks = _scan_tokens(text, line)
        if len(toks) != 3 or toks[1] != ("raw", "->"):
            self._fail(line, "ELSE syntax is 'ELSE -> <target>'")
        kind, target = toks[2]
        if kind == "raw" and target.startswith("."):
            return Node("else", line, target=None, label=target)
        if kind != "raw":
            self._fail(line, "ELSE target must be a number or a label")
        try:
            value = int(target, 0)
        except ValueError:
            self._fail(line, "bad ELSE target %r" % target)
        if value < 0:
            self._fail(line, "negative ELSE target")
        return Node("else", line, target=value, label=None)

    def _parse_eval(self, text, line):
        toks = _scan_tokens(text, line)
        if len(toks) < 2:
            self._fail(line, "EVAL without a condition name")
        name = toks[1][1]
        cid, sig = self._cond(name, line)
        args = _operands(toks[2:], sig, line, what="condition " + name)
        return Node("eval", line, cid=cid, name=name, args=args)

    def _cond(self, name, line):
        upper = name.upper()
        if upper in COND_BY_NAME:
            return COND_BY_NAME[upper]
        m = _OP_COND_RE.match(upper)
        if m:
            cid = int(m.group(1))
            if cid > 0xFF:
                self._fail(line, "condition id %d does not fit in a byte" % cid)
            return cid, ""
        self._fail(line, "unknown condition %r" % name)

    def _next_content(self, what):
        while self.i < len(self.lines) and not self._text(self.i):
            self.i += 1
        if self.i >= len(self.lines):
            raise AsmError(len(self.lines), "unexpected end of text in " + what)
        return self.i + 1, self._text(self.i)

    def _parse_table(self, text, line, name):
        m = _TABLE_HDR_RE.match(text)
        count = int(m.group(2))
        if count > 0x1000:
            self._fail(line, "%s count %d is too large" % (name, count))
        op = _TABLE_BY_NAME[name]
        sig = sbtvm.TABLE_SIGS[op]
        nstr = len(sig) - len(sig.lstrip("S"))
        letters = sig[nstr:]
        self.i += 1
        records = []
        for _ in range(count):
            rec_line, rec_text = self._next_content(name)
            if len(letters):
                parts = rec_text.rsplit(None, len(letters))
                if len(parts) != len(letters) + 1:
                    raise AsmError(
                        rec_line, "%s record needs %d value(s)"
                        % (name, len(letters)))
                head = parts[0]
                vals = [_operand(("raw", tok), letter, rec_line)
                        for tok, letter in zip(parts[1:], letters)]
            else:
                head = rec_text
                vals = []
            parts = _split_pipe(head.rstrip(), rec_line)
            if len(parts) != nstr:
                raise AsmError(
                    rec_line, "%s record: expected %d string(s), got %d"
                    % (name, nstr, len(parts)))
            strs = [_table_string(p, rec_line) for p in parts]
            records.append((strs, vals))
            self.i += 1
        return Node("table", line, name=name, op=op, sig=sig, records=records)

    def _parse_objectives(self, text, line):
        toks = _scan_tokens(text, line)[1:]
        strings = []
        for kind, tok in toks:
            if kind != "str":
                self._fail(line, "DEFINE_LEVEL_OBJECTIVES needs quoted strings")
            strings.append(_unquote(tok, line))
        if len(strings) > 30:
            self._fail(line, "DEFINE_LEVEL_OBJECTIVES accepts at most 30 strings")
        return Node("objectives", line, strings=strings)

    def _parse_path(self, text, line):
        m = _PATH_RE.match(text)
        if not m:
            self._fail(line, "SOURCEPLAYER_PATH syntax is 'n=<count> <values...>'")
        count = int(m.group(1))
        if count > 32:
            self._fail(line, "SOURCEPLAYER_PATH accepts at most 32 points")
        rest = _scan_tokens(m.group(2), line)
        if len(rest) != count:
            self._fail(line, "SOURCEPLAYER_PATH n=%d but %d value(s)"
                       % (count, len(rest)))
        vals = [_parse_int(tok[1], 32, line) for tok in rest]
        return Node("path", line, values=vals)

    def _parse_lift(self, text, line):
        m = _LIFT_RE.match(text)
        if not m:
            self._fail(line, "ADD_LIFT syntax is 'n=<count> [head=<hex>]'")
        count = int(m.group(1))
        if count > 0x1000:
            self._fail(line, "ADD_LIFT stop count %d is too large" % count)
        rest = _scan_tokens(m.group(2), line)
        head = None
        for kind, tok in rest:
            if kind != "raw":
                self._fail(line, "ADD_LIFT: unexpected token %r" % tok)
            if tok.startswith("head="):
                hexpart = tok[5:]
                if len(hexpart) != 0x94 * 2 or not re.match(r"^[0-9a-fA-F]*$", hexpart):
                    self._fail(
                        line, "ADD_LIFT head= must be %d hex digits (0x94 bytes)"
                        % (0x94 * 2))
                head = bytes.fromhex(hexpart)
            else:
                self._fail(line, "ADD_LIFT: unknown option %r" % tok)
        self.i += 1
        if head is None:
            if not self.allow_lossy:
                raise AsmError(
                    line,
                    "ADD_LIFT: gm.sbtvm.render() omits the 0x94-byte header "
                    "and the stop values; re-render with --exact or add "
                    "head=<hex> and the stop lines")
            return Node("lift", line, head=bytes(0x94),
                        stops=[(0, 0, 0)] * count)
        stops = []
        for _ in range(count):
            rec_line, rec_text = self._next_content("ADD_LIFT")
            toks = _scan_tokens(rec_text, rec_line)
            if len(toks) != 3:
                raise AsmError(rec_line, "ADD_LIFT stop needs 3 values")
            stops.append(tuple(_parse_int(tok[1], 32, rec_line) for tok in toks))
            self.i += 1
        return Node("lift", line, head=head, stops=stops)

    def _parse_db(self, text, line):
        data = bytearray()
        for kind, tok in _scan_tokens(text, line)[1:]:
            if kind == "str":
                value = _unquote(tok, line)
                try:
                    data += value.encode("latin1")
                except UnicodeEncodeError:
                    self._fail(line, "DB string is not latin-1")
            else:
                data.append(_parse_int(tok, 8, line))
        if not data:
            self._fail(line, "DB without bytes")
        return Node("db", line, data=bytes(data))

    def _parse_raw(self, text, line):
        toks = _scan_tokens(text, line)[1:]
        if len(toks) != 1:
            self._fail(line, "RAW needs exactly one hex string")
        kind, tok = toks[0]
        if kind == "str":
            tok = _unquote(tok, line)
        hexpart = "".join(tok.split())
        if len(hexpart) % 2 or not re.match(r"^[0-9a-fA-F]+$", hexpart):
            self._fail(line, "RAW expects an even-length hex string")
        return Node("raw", line, data=bytes.fromhex(hexpart))


class Emitter(object):
    """Turn the parsed nodes into bytes, patching labels/IF sizes."""

    def __init__(self):
        self.buf = bytearray()
        self.labels = {}
        self.fixups = []        # (field_pos, target, is_label, line)

    def run(self, nodes):
        self._nodes(nodes)
        total = len(self.buf)
        for pos, target, is_label, line in self.fixups:
            if is_label:
                if target not in self.labels:
                    raise AsmError(line, "unknown label %r" % target)
                tgt = self.labels[target]
            else:
                tgt = target
            if tgt > total:
                raise AsmError(line, "ELSE target 0x%x is past the end of the "
                                    "block (0x%x bytes)" % (tgt, total))
            rel = tgt - pos
            if rel < 4:
                raise AsmError(
                    line, "ELSE target 0x%x is not forward of the jump "
                    "(offset %d)" % (tgt, pos - 1))
            struct.pack_into("<I", self.buf, pos, rel)
        return bytes(self.buf)

    def _nodes(self, nodes):
        for nd in nodes:
            kind = nd.kind
            if kind == "label":
                if nd.name in self.labels:
                    raise AsmError(nd.line, "duplicate label %r" % nd.name)
                self.labels[nd.name] = len(self.buf)
            elif kind == "end":
                self.buf.append(0xFF)
            elif kind == "insn":
                self.buf.append(nd.op)
                self._args(nd.sig, nd.args, nd.line)
            elif kind == "eval":
                self.buf.append(0xC9)
                self.buf.append(nd.cid)
                self._args(self._cond_sig(nd), nd.args, nd.line)
            elif kind == "if":
                self.buf.append(199)
                self.buf.append(nd.cid)
                self._args(self._cond_sig(nd), nd.args, nd.line)
                size_pos = len(self.buf)
                self.buf += b"\0\0\0\0"
                body_start = len(self.buf)
                self._nodes(nd.body)
                struct.pack_into("<I", self.buf, size_pos,
                                 4 + len(self.buf) - body_start)
                if nd.else_body is not None:
                    self.buf.append(200)
                    rel_pos = len(self.buf)
                    self.buf += b"\0\0\0\0"
                    else_start = len(self.buf)
                    self._nodes(nd.else_body)
                    struct.pack_into("<I", self.buf, rel_pos,
                                     4 + len(self.buf) - else_start)
            elif kind == "else":
                self.buf.append(200)
                if nd.label is not None:
                    self.fixups.append((len(self.buf), nd.label, True, nd.line))
                else:
                    self.fixups.append((len(self.buf), nd.target, False, nd.line))
                self.buf += b"\0\0\0\0"
            elif kind == "table":
                self.buf.append(nd.op)
                self.buf += struct.pack("<I", len(nd.records))
                nstr = len(nd.sig) - len(nd.sig.lstrip("S"))
                letters = nd.sig[nstr:]
                for strs, vals in nd.records:
                    for s in strs:
                        self.buf += _string_bytes(s, nd.line)
                    self._args(letters, vals, nd.line)
            elif kind == "objectives":
                self.buf.append(103)
                self.buf += struct.pack("<I", len(nd.strings))
                for s in nd.strings:
                    self.buf += _string_bytes(s, nd.line)
            elif kind == "path":
                self.buf.append(158)
                self.buf += struct.pack("<I", len(nd.values))
                for v in nd.values:
                    self.buf += struct.pack("<I", v)
            elif kind == "lift":
                if len(nd.head) != 0x94:
                    raise AsmError(nd.line, "ADD_LIFT head must be 0x94 bytes")
                self.buf.append(188)
                self.buf += nd.head
                self.buf += struct.pack("<I", len(nd.stops))
                for stop in nd.stops:
                    for v in stop:
                        self.buf += struct.pack("<I", v)
            elif kind in ("db", "raw"):
                self.buf += nd.data
            else:                                   # pragma: no cover
                raise AsmError(nd.line, "internal: unknown node %r" % kind)

    def _cond_sig(self, nd):
        return sbtvm.CONDITIONS.get(nd.cid, (None, None))[1] or ""

    def _args(self, sig, args, line):
        if len(args) != len(sig):
            raise AsmError(line, "internal: %d value(s) for signature %r"
                           % (len(args), sig))
        for letter, value in zip(sig, args):
            if letter == "S":
                self.buf += _string_bytes(value, line)
            elif letter in "ABD":
                self.buf += struct.pack("<d", value)
            else:
                self.buf += struct.pack("<I" if _LETTER_SIZE[letter] == 4
                                        else "<B", value)


def assemble(text, allow_lossy=False):
    """Assemble text in the ``gm.sbtvm.render`` dialect into block bytes."""
    nodes = Parser(text, allow_lossy=allow_lossy).parse()
    return Emitter().run(nodes)


# ---------------------------------------------------------------------------
# exact renderer (same text as ``gm.sbtvm.render`` minus its two losses)


def _fmt_int(value):
    if value >= 0x10000:
        return "%d(0x%x)" % (value, value)
    return str(value)


def _fmt_dbl(value):
    if math.isfinite(value):
        return repr(float(value))
    return "0x" + struct.pack("<d", value).hex()


def _fmt_exact(value):
    if isinstance(value, str):
        return _escape(value)
    if isinstance(value, float):
        return _fmt_dbl(value)
    if isinstance(value, int):
        return _fmt_int(value)
    return str(value)


def _escape(value):
    out = ['"']
    for ch in value:
        code = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif code < 32 or code == 127:
            out.append("\\x%02x" % code)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def _table_string_out(value):
    if (not value or value != value.strip() or " | " in value
            or '"' in value or "\\" in value):
        return _escape(value)
    return value


def _arg_bytes(sig, args):
    """Byte length of decoded operands (strings are variable-length)."""
    total = 0
    for letter, value in zip(sig, args):
        if letter == "S":
            total += len(value.encode("latin1")) + 1
        else:
            total += _LETTER_SIZE[letter]
    return total


def render_exact(code, start=0, end=None, indent=0):
    """Byte-preserving variant of :func:`gm.sbtvm.render`.

    The text is produced by a **linear** walk over the bytes, not over the
    decode tree: ``gm.sbtvm.decode`` may visit the same range twice when an
    ``ELSE`` jumps past its enclosing ``IF`` (the engine's compiler puts the
    ``ELSE`` last in the then-branch and the else-code right after it).  A
    linear walk emits every byte exactly once, which is what an assembler
    needs; the flat ``ELSE -> 0x...`` form is used whenever the else-code does
    not fit inside the current scope.
    """
    scope_end = len(code) if end is None else end
    insns, _end = sbtvm.decode(code, start, end)
    index = {}
    for ins in _walk(insns):
        index.setdefault(ins.off, ins)
    lines, pos = _render_linear(code, start, scope_end, indent, index, True)
    if pos != scope_end:
        raise ValueError("render_exact: stopped at 0x%x of 0x%x"
                         % (pos, scope_end))
    return "\n".join(lines)


def insn_bytes(ins):
    """Encoded size of one decoded instruction, in bytes."""
    op = ins.op
    if op == 0xFF:
        return 1
    if op == 200:
        return 5
    if op == 199:
        (cname, cargs), = ins.args
        # the size field counts itself: total = op + cid + args + size
        return 2 + _arg_bytes(_cond_sig(cname), cargs) + ins.data
    if op == 0xC9:
        (cname, cargs), = ins.args
        return 2 + _arg_bytes(_cond_sig(cname), cargs)
    if op in sbtvm.TABLE_SIGS:
        sig = sbtvm.TABLE_SIGS[op]
        nstr = len(sig) - len(sig.lstrip("S"))
        letters = sig[nstr:]
        total = 5
        for strs, _vals in ins.data:
            for text in strs:
                total += len(text.encode("latin1")) + 1
            total += sum(_LETTER_SIZE[c] for c in letters)
        return total
    if op == 103:
        return 5 + sum(len(s.encode("latin1")) + 1 for s in ins.args)
    if op == 158:
        return 5 + 4 * ins.args[0][1]
    if op == 188:
        return 1 + 0x94 + 4 + 12 * ins.args[0][1]
    _name, sig = sbtvm.COMMANDS[op]
    return 1 + _arg_bytes(sig or "", ins.args)


def _render_linear(code, pos, scope_end, indent, index, top):
    pad = "    " * indent
    lines = []
    while pos < scope_end:
        ins = index.get(pos)
        if ins is None:
            raise ValueError("render_exact: no instruction at 0x%x "
                             "(hole in the decode)" % pos)
        size = insn_bytes(ins)
        if pos + size > scope_end:
            raise ValueError("render_exact: instruction at 0x%x (%s, %d bytes) "
                             "overruns the scope ending at 0x%x"
                             % (pos, ins.name, size, scope_end))
        if ins.name == "END":
            lines.append(pad + "---")
            pos += 1
            if top:
                break
            continue
        if ins.name in sbtvm._TABLES:
            lines.append("%s%s %d entries" % (pad, ins.name, len(ins.data)))
            for strs, vals in ins.data:
                lines.append("%s    %-24s %s" % (
                    pad, " | ".join(_table_string_out(s) for s in strs),
                    " ".join(_fmt_exact(v) for v in vals)))
        elif ins.name == "DEFINE_LEVEL_OBJECTIVES":
            lines.append(pad + "DEFINE_LEVEL_OBJECTIVES " +
                         " ".join(_escape(s) for s in ins.args))
        elif ins.name == "SOURCEPLAYER_PATH":
            lines.append("%sSOURCEPLAYER_PATH n=%d %s" % (
                pad, ins.args[0][1],
                " ".join(_fmt_exact(x) for x in ins.data)))
        elif ins.name == "ADD_LIFT":
            head, recs = ins.data
            lines.append("%sADD_LIFT n=%d head=%s" % (
                pad, ins.args[0][1], head.hex()))
            for rec in recs:
                lines.append("%s    %s" % (
                    pad, " ".join(_fmt_exact(x) for x in rec)))
        elif ins.name == "IF":
            (cname, cargs), = ins.args
            head = "%sIF %s" % (pad, cname)
            if cargs:
                head += " " + " ".join(_fmt_exact(x) for x in cargs)
            lines.append(head + " {")
            body_start = pos + 2 + _arg_bytes(_cond_sig(cname), cargs) + 4
            body_end = pos + 2 + _arg_bytes(_cond_sig(cname), cargs) + ins.data
            if body_end > scope_end:
                raise ValueError("render_exact: IF body at 0x%x ends at 0x%x, "
                                 "past the scope end 0x%x"
                                 % (pos, body_end, scope_end))
            body_lines, _p = _render_linear(
                code, body_start, body_end, indent + 1, index, False)
            lines.extend(body_lines)
            if (body_end + 5 <= scope_end and code[body_end] == 200):
                rel = struct.unpack_from("<I", code, body_end + 1)[0]
                target = body_end + 1 + rel
                if target <= scope_end:
                    lines.append(pad + "} else {")
                    else_lines, _q = _render_linear(
                        code, body_end + 5, target, indent + 1, index, False)
                    lines.extend(else_lines)
                    lines.append(pad + "}")
                    pos = target
                    continue
            lines.append(pad + "}")
            pos = body_end
            continue
        elif ins.name == "ELSE":
            lines.append("%sELSE -> 0x%x" % (pad, ins.args[0][1]))
            pos += 5
            continue
        elif ins.name == "EVAL":
            (cname, cargs), = ins.args
            head = "%sEVAL %s" % (pad, cname)
            if cargs:
                head += " " + " ".join(_fmt_exact(x) for x in cargs)
            lines.append(head)
        else:
            lines.append(("%s%-34s %s" % (
                pad, ins.name, " ".join(_fmt_exact(x) for x in ins.args))).rstrip())
        pos += size
    return lines, pos


def _cond_sig(name):
    for _cid, (cname, sig) in sbtvm.CONDITIONS.items():
        if cname == name:
            return sig or ""
    return ""


# ---------------------------------------------------------------------------
# blocks of a level


def level_path(name, game=DEFAULT_GAME):
    """Resolve a path or a level stem (``dm1``) to an existing ``.sbt``."""
    if os.path.isfile(name):
        return name
    for cand in (name, name + ".sbt"):
        path = os.path.join(game, cand)
        if os.path.isfile(path):
            return path
    return None


def script_of(sbt):
    recs = sbt.records_of(11)
    if not recs:
        raise ValueError("no SCRIPT record (type 11)")
    return recs[0].payload


def block_starts(script):
    """Sub-script entry offsets: main (0) plus the SCRIPT handle table."""
    n = script.n
    starts = [0]
    if n > 0 and len(script.handles) >= 12 * n:
        col = struct.unpack_from("<%dI" % n, script.handles, 2 * 4 * n)
        starts.extend(col)
    return starts


def block_spans(code, starts):
    """``[(start, end)]`` for the given entry offsets, in byte order.

    A sub-script ends at its top-level ``END`` (0xFF).  One pass over the code
    gives every instruction boundary, so the whole corpus is cheap.
    """
    top, _end = sbtvm.decode(code)
    index = {}
    for k, ins in enumerate(top):
        index.setdefault(ins.off, k)
    spans = []
    seen = set()
    for start in sorted(set(starts)):
        if start in seen:
            continue
        seen.add(start)
        k = index.get(start)
        if k is None:
            # not an instruction boundary of the linear decode: decode it alone
            insns, _ = sbtvm.decode(code, start)
            for ins in insns:
                if ins.name == "END":
                    spans.append((start, ins.off + 1))
                    break
            else:
                raise ValueError("block at 0x%x has no END" % start)
            continue
        j = k
        while j < len(top) and top[j].name != "END":
            j += 1
        if j >= len(top):
            raise ValueError("block at 0x%x has no END" % start)
        spans.append((start, top[j].off + 1))
    spans.sort()
    return spans


def extract_blocks(code, starts):
    return [(s, e, code[s:e]) for s, e in block_spans(code, starts)]


# ---------------------------------------------------------------------------
# corpus round-trip


def _walk(insns):
    for ins in insns:
        yield ins
        if isinstance(ins.body, list):
            yield from _walk(ins.body)


def _first_diff(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return min(len(a), len(b))
    return -1


def census(code, start=0, end=None, insns=None):
    """Instruction census of one block, walking the bytes linearly.

    A linear walk visits every byte once: ``gm.sbtvm.decode`` can return the
    same range twice when an ``ELSE`` jumps past its enclosing ``IF`` (see
    :func:`render_exact`), so the decode tree would double-count those
    instructions.
    """
    if end is None:
        end = len(code)
    if insns is None:
        insns, _end = sbtvm.decode(code, start, end)
    index = {}
    for ins in _walk(insns):
        index.setdefault(ins.off, ins)
    feat = {
        "lossy_float": 0,
        "lift": 0,
        "tables": 0,
        "insns": 0,
        "max_depth": 0,
        "opcodes": collections.Counter(),
        "conditions": collections.Counter(),
    }
    stack = []
    pos = start
    while pos < end:
        ins = index.get(pos)
        if ins is None:
            raise ValueError("census: no instruction at 0x%x" % pos)
        while stack and pos >= stack[-1]:
            stack.pop()
        feat["insns"] += 1
        feat["max_depth"] = max(feat["max_depth"], len(stack))
        feat["opcodes"][ins.name] += 1
        values = list(ins.args) if isinstance(ins.args, list) else []
        if ins.op in (199, 201):
            cname, cargs = ins.args[0]
            feat["conditions"][cname] += 1
            values.extend(cargs)
        if ins.name == "ADD_LIFT":
            feat["lift"] += 1
        if ins.name in sbtvm._TABLES:
            feat["tables"] += 1
            for _strs, vals in ins.data:
                values.extend(vals)
        for value in values:
            if isinstance(value, float) and float("%g" % value) != value:
                feat["lossy_float"] += 1
        if ins.name == "IF":
            (cname, cargs), = ins.args
            header = 2 + _arg_bytes(_cond_sig(cname), cargs) + 4
            body_end = pos + header + (ins.data - 4)
            stack.append(body_end)
            pos += header
            continue
        if ins.name == "ELSE":
            rel = struct.unpack_from("<I", code, pos + 1)[0]
            stack.append(pos + 1 + rel)
            pos += 5
            continue
        pos += insn_bytes(ins)
    return feat


def _ref_reason(feat, err):
    if err:
        return "asm-error: " + str(err)
    parts = []
    if feat["lossy_float"]:
        parts.append("%d double field(s) lose digits with %%g"
                     % feat["lossy_float"])
    if feat["lift"]:
        parts.append("ADD_LIFT header/stops not printed by render()")
    return "; ".join(parts) or "unknown"


def _ref_cause(feat, err):
    """Coarse bucket for a reference-render round-trip failure."""
    if err:
        text = str(err)
        if "ELSE target" in text or "IF " in text or "unexpected" in text:
            return "structural-nesting"
        return "asm-error"
    kinds = []
    if feat["lossy_float"]:
        kinds.append("float-g")
    if feat["lift"]:
        kinds.append("add-lift")
    return "+".join(kinds) or "other"


_CAUSE_TEXT = {
    "float-g": "double printed with %g (6 significant digits)",
    "add-lift": "ADD_LIFT header/stops not printed",
    "float-g+add-lift": "both of the above",
    "structural-nesting": "ELSE body lies outside the enclosing IF's size; "
                          "render nests it inside",
    "other": "unclassified",
}


def reference_loss(block):
    """``None`` if ``gm.sbtvm.render`` round-trips this block, else a reason."""
    try:
        out = assemble(sbtvm.render(block), allow_lossy=True)
    except AsmError as exc:
        return str(exc)
    if out == block:
        return None
    return _ref_reason(census(block), None)


def roundtrip_corpus(game=DEFAULT_GAME, modes=("exact", "reference"),
                     limit=0, verbose=False):
    paths = sorted(glob.glob(os.path.join(game, "*.sbt")))
    started = time.time()
    totals = {
        "levels": 0,
        "blocks": 0,
        "bytes": 0,
        "modes": {m: {"ok": 0, "mismatch": 0, "error": 0} for m in modes},
        "opcodes": collections.Counter(),
        "conditions": collections.Counter(),
        "insns": 0,
        "max_depth": 0,
        "blocks_with_tables": 0,
        "blocks_with_lift": 0,
        "blocks_with_lossy_float": 0,
        "decode_exact": 0,
        "reasons": collections.Counter(),
        "causes": collections.Counter(),
        "level_errors": [],
        "mismatches": [],
        "levels_report": [],
    }
    for path in paths:
        sbt = sbtmod.parse_file(path)
        script = script_of(sbt)
        code = script.code
        try:
            blocks = extract_blocks(code, block_starts(script))
        except (ValueError, struct.error) as exc:
            totals["level_errors"].append(
                {"level": os.path.basename(path), "error": str(exc)})
            continue
        lvl = {
            "level": os.path.basename(path),
            "blocks": len(blocks),
            "bytes": sum(len(blk) for _s, _e, blk in blocks),
        }
        for mode in modes:
            lvl[mode] = 0
        totals["levels"] += 1
        for start, _end, blk in blocks:
            if limit and totals["blocks"] >= limit:
                break
            totals["blocks"] += 1
            totals["bytes"] += len(blk)
            insns, consumed = sbtvm.decode(blk)
            if consumed == len(blk):
                totals["decode_exact"] += 1
            feat = census(blk, 0, len(blk), insns)
            totals["insns"] += feat["insns"]
            totals["max_depth"] = max(totals["max_depth"], feat["max_depth"])
            totals["opcodes"].update(
                {k: v for k, v in feat["opcodes"].items()})
            totals["conditions"].update(feat["conditions"])
            if feat["tables"]:
                totals["blocks_with_tables"] += 1
            if feat["lift"]:
                totals["blocks_with_lift"] += 1
            if feat["lossy_float"]:
                totals["blocks_with_lossy_float"] += 1
            for mode in modes:
                if mode == "exact":
                    try:
                        text = render_exact(blk)
                        out = assemble(text)
                        err = None
                    except (AsmError, ValueError, struct.error) as exc:
                        out, err = b"", exc
                else:
                    try:
                        text = sbtvm.render(blk)
                        out = assemble(text, allow_lossy=True)
                        err = None
                    except (AsmError, ValueError, struct.error) as exc:
                        out, err = b"", exc
                if err is None and out == blk:
                    totals["modes"][mode]["ok"] += 1
                    lvl[mode] += 1
                    continue
                diff = _first_diff(blk, out) if err is None else -1
                reason = _ref_reason(feat, err)
                cause = _ref_cause(feat, err)
                totals["causes"][cause] += 1
                if mode == "exact":
                    reason = "EXACT RENDER MISMATCH at +0x%x (%s)" % (diff, reason)
                totals["modes"][mode]["error" if err else "mismatch"] += 1
                totals["reasons"][reason] += 1
                totals["mismatches"].append({
                    "level": os.path.basename(path),
                    "block": "0x%x" % start,
                    "bytes": len(blk),
                    "mode": mode,
                    "first_diff": diff,
                    "reason": reason,
                    "cause": cause,
                })
        totals["levels_report"].append(lvl)
        if limit and totals["blocks"] >= limit:
            break
    totals["seconds"] = round(time.time() - started, 2)
    totals["opcodes"] = dict(totals["opcodes"])
    totals["conditions"] = dict(totals["conditions"])
    totals["reasons"] = dict(totals["reasons"])
    totals["causes"] = dict(totals["causes"])
    # the mismatch list is noisy by design; the summary keeps the first 20
    totals["mismatch_count"] = len(totals["mismatches"])
    if not verbose:
        totals["mismatches"] = totals["mismatches"][:20]
    return totals


# ---------------------------------------------------------------------------
# self-test (no game files)


def _pack_value(letter, value):
    if letter == "S":
        return value.encode("latin1") + b"\0"
    if letter in "ABD":
        return struct.pack("<d", value)
    return struct.pack("<I" if _LETTER_SIZE[letter] == 4 else "<B", value)


def _pack_insn(op, args, sig):
    out = bytearray([op])
    for letter, value in zip(sig, args):
        out += _pack_value(letter, value)
    return bytes(out)


def _pack_ev(prefix, cid, args, sig, body=b"", else_body=None):
    """Build an IF/EVAL instruction (``prefix`` = 199 or 0xC9)."""
    out = bytearray([prefix, cid])
    for letter, value in zip(sig, args):
        out += _pack_value(letter, value)
    if prefix == 0xC9:
        return bytes(out)
    out += struct.pack("<I", 4 + len(body))
    out += body
    if else_body is not None:
        out += b"\xc8"
        out += struct.pack("<I", 4 + len(else_body))
        out += else_body
    return bytes(out)


def _nice_args(sig):
    out = []
    for k, letter in enumerate(sig):
        if letter == "S":
            out.append("str%d" % k)
        elif letter in "ABD":
            out.append(0.5 * (k + 1) if k % 2 == 0 else -2.25)
        elif letter == "Y":
            out.append(3)
        else:
            out.append(5 + k)
    return out


def _table_bytes(op, sig, records):
    out = bytearray([op])
    out += struct.pack("<I", len(records))
    nstr = len(sig) - len(sig.lstrip("S"))
    letters = sig[nstr:]
    for rec in records:
        for s in rec[:nstr]:
            out += s.encode("latin1") + b"\0"
        for letter, value in zip(letters, rec[nstr:]):
            out += _pack_value(letter, value)
    return bytes(out)


def _selftest():
    checks = []

    def check(name, fn):
        try:
            fn()
            checks.append((name, True, ""))
        except Exception as exc:                    # noqa: BLE001
            checks.append((name, False, "%s: %s" % (type(exc).__name__, exc)))

    def eq(got, want, what=""):
        if got != want:
            raise AssertionError("%s: got %r, want %r" % (what or "value",
                                                          got, want))

    def roundtrip(blk, mode="exact"):
        if mode == "exact":
            return assemble(render_exact(blk))
        return assemble(sbtvm.render(blk), allow_lossy=True)

    # 1. every plain command through the reference renderer
    def t_commands():
        for op in sorted(sbtvm.COMMANDS):
            if op in sbtvm.TABLE_SIGS:
                continue
            name, sig = sbtvm.COMMANDS[op]
            sig = sig or ""
            args = _nice_args(sig)
            blk = _pack_insn(op, args, sig)
            eq(roundtrip(blk), blk, name)
            eq(roundtrip(blk, "reference"), blk, name + " (reference)")

    # 2. every condition in IF and EVAL
    def t_conditions():
        for cid in sorted(sbtvm.CONDITIONS):
            name, sig = sbtvm.CONDITIONS[cid]
            sig = sig or ""
            args = _nice_args(sig)
            blk = _pack_ev(199, cid, args, sig, body=b"")
            eq(roundtrip(blk), blk, name + " IF")
            blk = _pack_ev(0xC9, cid, args, sig)
            eq(roundtrip(blk), blk, name + " EVAL")

    # 3. doubles: the reference renderer loses digits, the exact one does not
    def t_double_precision():
        value = 0.034906585039886584
        blk = _pack_insn(77, [value, 1, 2], "APP") + b"\xff"
        ref = roundtrip(blk, "reference")
        eq(ref == blk, False, "reference render must be lossy here")
        eq(roundtrip(blk), blk, "exact float")
        again = assemble(render_exact(blk))
        eq(again, blk, "exact float, second pass")
        bits = struct.pack("<d", value).hex()
        text = render_exact(blk).replace(repr(value), "0x" + bits)
        eq(assemble(text), blk, "hex bit form")
        zero = _pack_insn(96, [-0.0], "D")
        eq(roundtrip(zero), zero, "-0.0")
        # a lossy double inside a DEFINE_PLAYER_TYPES record
        sig = sbtvm.TABLE_SIGS[172]
        nstr = len(sig) - len(sig.lstrip("S"))
        letters = sig[nstr:]
        rec = tuple("s%d" % k for k in range(nstr)) + tuple(
            value if letter in "ABD" else 3 for letter in letters)
        table = _table_bytes(172, sig, [rec])
        eq(roundtrip(table), table, "lossy double in table")

    # 4. all four DEFINE_* tables, including awkward strings
    def t_tables():
        for op in sorted(sbtvm.TABLE_SIGS):
            name = sbtvm.TABLE_NAMES[op]
            sig = sbtvm.TABLE_SIGS[op]
            nstr = len(sig) - len(sig.lstrip("S"))
            letters = sig[nstr:]
            recs = []
            refrecs = []
            for r in range(2):
                strs = ["s%d_%d" % (r, k) for k in range(nstr)]
                refstrs = list(strs)
                if nstr:
                    strs[0] = "POWER CELLS" if r == 0 else ""
                    refstrs[0] = "POWER CELLS"
                vals = [0.5 if c in "ABD" else (r + 1) for c in letters]
                recs.append(tuple(strs) + tuple(vals))
                refrecs.append(tuple(refstrs) + tuple(vals))
            blk = _table_bytes(op, sig, recs)
            eq(roundtrip(blk), blk, name)
            refblk = _table_bytes(op, sig, refrecs)
            eq(roundtrip(refblk, "reference"), refblk, name + " (reference)")

    # 5. objectives, path, lift, END
    def t_structural():
        # the structural table must stay in sync with gm.sbtvm
        for op, name in STRUCT_OPS.items():
            eq(op in sbtvm.COMMANDS, False, "structural op %d in COMMANDS" % op)
            eq(bool(name), True)
        eq(len(STRUCT_OPS), 6, "structural opcode count")
        blk = (b"\x67" + struct.pack("<I", 3) + b"one\0"
               b'two "quoted"\0' + b"back\\slash\0" + b"\xff")
        eq(roundtrip(blk), blk, "DEFINE_LEVEL_OBJECTIVES")
        blk = (b"\x67" + struct.pack("<I", 0) + b"\xff")
        eq(roundtrip(blk), blk, "DEFINE_LEVEL_OBJECTIVES n=0")
        blk = (b"\x9e" + struct.pack("<I", 4) + struct.pack("<4I", 0, 1, 2,
                                                          0xFFFFFFFF) + b"\xff")
        eq(roundtrip(blk), blk, "SOURCEPLAYER_PATH")
        head = bytes(range(0x94))
        blk = (b"\xbc" + head + struct.pack("<I", 2)
               + struct.pack("<6I", 7, 8, 9, 0xFFFFFFFF, 11, 12) + b"\xff")
        eq(roundtrip(blk), blk, "ADD_LIFT")
        ref = roundtrip(blk, "reference")
        eq(ref == blk, False, "reference render must be lossy here")
        try:
            assemble(sbtvm.render(blk))
        except AsmError as exc:
            if "ADD_LIFT" not in str(exc):
                raise AssertionError("unexpected ADD_LIFT error text: %s" % exc)
        else:
            raise AssertionError("ADD_LIFT without head= must fail")

    # 6. control flow: nested IF/ELSE, empty then, labels
    def t_flow():
        inner = _pack_insn(83, [], "")          # STOP_MUSIC
        mid = _pack_ev(199, 9, [], "", body=inner, else_body=inner)
        outer = _pack_ev(199, 12, [], "", body=mid + b"\xff",
                         else_body=inner + b"\xff")
        blk = outer + b"\xff"
        eq(roundtrip(blk), blk, "nested IF/ELSE")
        eq(roundtrip(blk, "reference"), blk, "nested IF/ELSE (reference)")
        empty = _pack_ev(199, 9, [], "", body=b"")
        eq(roundtrip(empty), empty, "empty then")
        text = "IF SOURCE_IS_PLAYER {\n}\n"
        eq(assemble(text), empty, "empty then text")
        unknown = bytes([199, 77]) + struct.pack("<I", 4) + b"\xff"
        eq(assemble("IF OP_77 {\n}\n---\n"), unknown, "unknown condition id")
        eq(roundtrip(unknown), unknown, "unknown condition id round-trip")
        # labels and numeric targets must agree
        numeric = _pack_insn(83, [], "") + b"\xc8" + struct.pack("<I", 4) + b"\xff"
        eq(roundtrip(numeric), numeric, "standalone ELSE rel=4")
        labelled = ("STOP_MUSIC\n.j:\nELSE -> .j\n---\n")
        try:
            assemble(labelled)
        except AsmError as exc:
            if "not forward" not in str(exc):
                raise AssertionError("unexpected error: %s" % exc)
        else:
            raise AssertionError("backward ELSE must fail")
        # a real forward label
        text = ("STOP_MUSIC\nELSE -> .tail\n.tail:\n---\n")
        got = assemble(text)
        insns, _ = sbtvm.decode(got)
        else_ins = [i for i in insns if i.name == "ELSE"][0]
        eq(else_ins.args[0][1], len(got) - 1, "ELSE target")

    # 7. DB / RAW escape hatches
    def t_raw():
        blk = b"\x01\x02\x03\x04\xff"
        eq(assemble("DB 1 2 3 4\n---\n"), blk, "DB")
        eq(assemble('DB "\\x01\\x02" 3 4\n---\n'), blk, "DB with string")
        eq(assemble("RAW 01020304ff\n"), blk, "RAW")

    # 8. number forms
    def t_numbers():
        want = _pack_insn(7, [0xFFFFFFFF], "P")
        for text in ("SLEEP 4294967295\n", "SLEEP 4294967295(0xffffffff)\n",
                     "SLEEP 0xffffffff\n", "SLEEP -1\n"):
            eq(assemble(text), want, text.strip())
        hexbits = struct.pack("<d", math.pi).hex()
        eq(assemble("SET_LEVEL_GRAVITY 0x%s\n" % hexbits),
           _pack_insn(96, [math.pi], "D"), "pi via bits")

    # 9. comments and labels
    def t_comments():
        text = ("; leading comment\n"
                "  # another\n"
                ".start:\n"
                "STOP_MUSIC   ; trailing\n"
                "---  // end\n")
        eq(assemble(text), b"\x53\xff", "comments")

    # 10. error reporting, with line numbers
    def t_errors():
        cases = [
            ("STOP_MUSIC\nNOPE 1\n", 2, "unknown command"),
            ("LOG_MESSAGE\n", 1, "operand"),
            ("SLEEP 1.5\n", 1, "integer"),
            ('LOG_MESSAGE "abc\n', 1, "unterminated"),
            ("IF SOURCE_IS_PLAYER {\nSTOP_MUSIC\n", 1, "IF without"),
            ("IF WEIRD_COND {\n}\n", 1, "unknown condition"),
            ("STOP_MUSIC\nELSE -> 0x0\n", 2, "not forward"),
            ("ELSE -> .nope\n", 1, "unknown label"),
            (".a:\n.a:\n", 2, "duplicate label"),
            ("DEFINE_AMMO 1 entries\nx\n", 2, "needs"),
            ("SOURCEPLAYER_PATH n=2 1\n", 1, "value"),
            ("RAW abc\n", 1, "hex"),
            ("SET_LEVEL_VERSION 4294967296\n", 1, "does not fit"),
            ("SLEEP \"x\"\n", 1, "string"),
        ]
        for text, line, needle in cases:
            try:
                assemble(text)
            except AsmError as exc:
                eq(exc.line, line, "line number for %r" % text.strip())
                if needle not in str(exc):
                    raise AssertionError("error text %r lacks %r"
                                         % (str(exc), needle))
            else:
                raise AssertionError("expected AsmError for %r" % text.strip())

    # 11. all bytes of a block survive a second exact pass
    def t_stability():
        text = "IF GAME_DIFFICULTY_AT_LEAST 3 {\n    EVAL RANDOM 0.5\n---\n}\n"
        first = assemble(text)
        second = assemble(render_exact(first))
        eq(second, first, "second pass")

    for name, fn in [
        ("commands: every opcode through render -> asm", t_commands),
        ("conditions: every IF/EVAL through render -> asm", t_conditions),
        ("doubles: exact render and 0x bit form", t_double_precision),
        ("tables: DEFINE_WEAPONS/AMMO/PLAYER_TYPES/EQUIPMENT", t_tables),
        ("structural: objectives/path/ADD_LIFT/END", t_structural),
        ("flow: nested IF/ELSE, empty then, labels", t_flow),
        ("raw: DB/RAW escape hatches", t_raw),
        ("numbers: decimal/hex/annotated/negative/bits", t_numbers),
        ("lexing: comments and labels", t_comments),
        ("errors: line numbers and messages", t_errors),
        ("stability: render -> asm -> render -> asm", t_stability),
    ]:
        check(name, fn)

    passed = sum(1 for _n, ok, _d in checks if ok)
    for name, ok, detail in checks:
        print("%s  %s%s" % ("ok  " if ok else "FAIL", name,
                           "" if ok else "  -- " + detail))
    print("%d/%d self-tests passed" % (passed, len(checks)))
    return passed == len(checks)


# ---------------------------------------------------------------------------
# CLI


def _print_roundtrip(rep, verbose=False):
    print("roundtrip: %d levels, %d blocks, %d bytes, %.2fs"
          % (rep["levels"], rep["blocks"], rep["bytes"], rep["seconds"]))
    print("  decode reaches block end exactly: %d/%d"
          % (rep["decode_exact"], rep["blocks"]))
    for mode in ("exact", "reference"):
        if mode not in rep["modes"]:
            continue
        m = rep["modes"][mode]
        total = m["ok"] + m["mismatch"] + m["error"]
        pct = 100.0 * m["ok"] / total if total else 0.0
        print("  %-9s: %d/%d byte-identical (%.2f%%), %d mismatch, %d error"
              % (mode, m["ok"], total, pct, m["mismatch"], m["error"]))
    print("  instructions: %d, max IF nesting: %d" % (rep["insns"],
                                                      rep["max_depth"]))
    print("  blocks with tables: %d, with ADD_LIFT: %d, with lossy doubles: %d"
          % (rep["blocks_with_tables"], rep["blocks_with_lift"],
             rep["blocks_with_lossy_float"]))
    print("  opcodes seen: %d names; conditions seen: %d"
          % (len(rep["opcodes"]), len(rep["conditions"])))
    if rep.get("level_errors"):
        print("  levels that failed to decode: %d" % len(rep["level_errors"]))
        for bad in rep["level_errors"][:5]:
            print("    %s: %s" % (bad["level"], bad["error"]))
    if rep.get("causes"):
        print("  failures by cause:")
        for cause, count in sorted(rep["causes"].items(),
                                   key=lambda kv: -kv[1]):
            print("    %5d  %-22s %s" % (count, cause,
                                         _CAUSE_TEXT.get(cause, cause)))
    if verbose and rep["reasons"]:
        print("  per-block reasons:")
        for reason, count in sorted(rep["reasons"].items(),
                                    key=lambda kv: -kv[1]):
            print("    %5d  %s" % (count, reason))
    shown = 0
    for mm in rep["mismatches"]:
        shown += 1
        if shown > 10 and not verbose:
            break
        print("    [%s] %s %s (%d bytes) diff +0x%x: %s"
              % (mm["mode"], mm["level"], mm["block"], mm["bytes"],
                 mm["first_diff"], mm["reason"]))
    if verbose:
        for lvl in rep["levels_report"]:
            print("    %-14s blocks %5d bytes %7d exact %5d ref %5d"
                  % (lvl["level"], lvl["blocks"], lvl["bytes"],
                     lvl.get("exact", 0), lvl.get("reference", 0)))


def _cmd_disasm(args):
    game = args.game or DEFAULT_GAME
    path = level_path(args.path, game)
    if not path:
        print("error: no such level: %s" % args.path, file=sys.stderr)
        return 2
    sbt = sbtmod.parse_file(path)
    script = script_of(sbt)
    code = script.code
    try:
        spans = block_spans(code, block_starts(script))
    except (ValueError, struct.error) as exc:
        print("error: %s: %s" % (os.path.basename(path), exc), file=sys.stderr)
        return 2
    wanted = []
    if args.block in (None, ""):
        wanted = spans[:1]
    elif str(args.block).lower() == "all":
        wanted = spans
    else:
        try:
            off = int(args.block, 0)
        except ValueError:
            print("error: bad --block %r" % args.block, file=sys.stderr)
            return 2
        span = next((s for s in spans if s[0] == off), None)
        if span is None:
            print("error: no block starts at 0x%x (use --block all or "
                  "check the handle table)" % off, file=sys.stderr)
            return 2
        wanted = [span]
    rendered = []
    notes = []
    for start, end in wanted:
        blk = code[start:end]
        try:
            text = render_exact(blk) if args.exact else sbtvm.render(blk)
        except (ValueError, struct.error) as exc:
            print("error: block 0x%x: %s" % (start, exc), file=sys.stderr)
            return 2
        if not args.exact:
            loss = reference_loss(blk)
            if loss:
                notes.append("block 0x%x: reference render is not "
                             "byte-preserving (%s); use --exact" % (start, loss))
        rendered.append((start, end, text))
    for note in notes:
        print("note: " + note, file=sys.stderr)
    if args.out:
        if len(rendered) == 1:
            with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(rendered[0][2] + "\n")
            target = args.out
        else:
            os.makedirs(args.out, exist_ok=True)
            for start, _end, text in rendered:
                name = "%08x.asm" % start
                with open(os.path.join(args.out, name), "w",
                          encoding="utf-8", newline="\n") as fh:
                    fh.write(text + "\n")
            target = args.out
        print("wrote %d block(s) to %s" % (len(rendered), target))
        return 0
    if args.json:
        payload = {
            "level": os.path.basename(path),
            "code_bytes": len(code),
            "blocks": [{"block": "0x%x" % s, "bytes": e - s,
                        "text": t} for s, e, t in rendered],
        }
        print(json.dumps(payload, indent=1))
        return 0
    for idx, (start, _end, text) in enumerate(rendered):
        if len(rendered) > 1:
            if idx:
                print()
            print("# ==== block 0x%x ====" % start)
        print(text)
    return 0


def _cmd_asm(args):
    path = args.asm
    if path == "-":
        text = sys.stdin.read()
        src = "<stdin>"
    else:
        if not os.path.isfile(path):
            print("error: no such file: %s" % path, file=sys.stderr)
            return 2
        with open(path, "rb") as fh:
            raw = fh.read()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("latin1")     # hand-edited files in a legacy codec
        src = path
    try:
        data = assemble(text, allow_lossy=args.allow_lossy)
    except AsmError as exc:
        print("%s: %s" % (src, exc), file=sys.stderr)
        if args.json:
            print(json.dumps({"source": src, "error": str(exc),
                              "line": exc.line}))
        return 1
    decode_ok = True
    decode_error = None
    try:
        _insns, consumed = sbtvm.decode(data)
        decode_ok = consumed == len(data)
        if not decode_ok:
            decode_error = ("decode stopped at 0x%x of 0x%x"
                            % (consumed, len(data)))
    except Exception as exc:                        # noqa: BLE001
        decode_ok = False
        decode_error = str(exc)
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        summary = "assembled %s -> %s (%d bytes)" % (src, args.out, len(data))
    else:
        summary = "assembled %s (%d bytes)" % (src, len(data))
    if args.json:
        print(json.dumps({
            "source": src,
            "bytes": len(data),
            "hex": data.hex(),
            "decode_ok": decode_ok,
            "decode_error": decode_error,
            "out": args.out,
        }, indent=1))
        return 0
    print(summary)
    if not decode_ok:
        print("warning: gm.sbtvm.decode() does not reach the end: %s"
              % decode_error, file=sys.stderr)
    if not args.out:
        print(data.hex())
    return 0


def _cmd_roundtrip(args):
    modes = ("exact", "reference")
    if args.render == "exact":
        modes = ("exact",)
    elif args.render == "reference":
        modes = ("reference",)
    game = args.game or DEFAULT_GAME
    rep = roundtrip_corpus(game, modes=modes, limit=args.limit,
                           verbose=args.verbose)
    if args.json:
        print(json.dumps(rep, indent=1))
        return 0
    _print_roundtrip(rep, args.verbose)
    bad = sum(rep["modes"][m]["mismatch"] + rep["modes"][m]["error"]
              for m in rep["modes"])
    return 0 if bad == 0 else 1


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # accept the documented flag spellings as pseudo-subcommands
    if argv:
        head = argv[0].lower()
        if head in ("self-test", "selftest", "--self-test", "--selftest"):
            argv = ["--self-test"] + argv[1:]
        elif head in ("roundtrip", "--roundtrip"):
            argv = ["--roundtrip"] + argv[1:]
        elif head in ("asm", "--asm") and len(argv) > 1:
            argv = ["--asm", argv[1]] + argv[2:]
        elif head == "disasm":
            argv = argv[1:]
    ap = argparse.ArgumentParser(
        description="Assembler for Gunmetal SCRIPT bytecode (.sbt record 11).")
    ap.add_argument("path", nargs="?", help="level .sbt (or stem) to disassemble")
    ap.add_argument("--block", help="block offset (0x.. or decimal) or 'all'")
    ap.add_argument("--asm", metavar="FILE", help="assemble this text file")
    ap.add_argument("--out", "-o", metavar="FILE",
                    help="write output here (text for disasm, bytes for asm, "
                         "directory with --block all)")
    ap.add_argument("--exact", action="store_true",
                    help="disasm: exact dialect (repr doubles, ADD_LIFT head)")
    ap.add_argument("--allow-lossy", action="store_true",
                    help="asm: zero-fill data that render() does not print")
    ap.add_argument("--roundtrip", action="store_true",
                    help="round-trip every block of the corpus")
    ap.add_argument("--render", choices=("exact", "reference", "both"),
                    default="exact",
                    help="roundtrip: which text dialect (default exact = the "
                         "100%% criterion; 'reference' measures "
                         "gm.sbtvm.render and shows its losses)")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic tests (no game files)")
    ap.add_argument("--game", default=None,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=0,
                    help="roundtrip: stop after N blocks (debugging)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if _selftest() else 1
    if args.roundtrip:
        return _cmd_roundtrip(args)
    if args.asm:
        return _cmd_asm(args)
    if args.path:
        return _cmd_disasm(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AsmError as exc:
        print("error: %s" % exc, file=sys.stderr)
        sys.exit(1)
    except (ValueError, struct.error, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        sys.exit(1)
