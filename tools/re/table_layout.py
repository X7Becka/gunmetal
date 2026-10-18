#!/usr/bin/env python3
"""Recover the in-memory layout of a compiled ``DEFINE_*`` table record.

The engine's table *store functions* take the whole record as a long argument
list and write every field into a fixed offset of a record array::

    mov  ecx,[esp+0x17c]              ; arg89
    mov  DWORD PTR [eax+0x7995e0],ecx ; memory +0x00 <- arg89

This script walks such a function in the objdump listing, pairs every store
with the ``[esp+N]`` it copies, converts the stack slot into an argument
index and prints ``memory offset <- record dword`` (the record dword order is
the operand order of the signature string, so it maps straight onto the file).

The store function is walked for ``mov [base+0xXXXX],reg`` and every store is
paired with the ``[esp+N]`` slot the register was loaded from.  With
``--handler LO HI`` the argument -> record dword map is instead derived from
the handler's push sequence (exact for tables whose handler pushes the record
in order; note that a handler which parks *extra* values -- string pointers,
converted handles -- in the middle of the list makes the naive ``arg-1``
index off by one for the region *before* that push).

Known store functions:
    0x47a170  DEFINE_PLAYER_TYPES (chassis)  base 0x7995e0, stride 0x17c
    0x450b10  DEFINE_AMMO                    base 0x818960, stride 0x38 (5 args)
    0x450be0  DEFINE_WEAPONS                 base 0x80fa00, stride 0x2a8
    0x450e60  DEFINE_EQUIPMENT               base 0x814c60, stride 0x40

Usage:  python3 table_layout.py 0x47a170 0x7995e0 [--end 0x47a730]
        python3 table_layout.py 0x47a170 0x7995e0 --stride 0x17c
"""
import argparse
import re
import sys

import exelib

# prologue pushes before the argument list is built; --esp-bias adjusts it
load = re.compile(r'mov\s+(\w+),DWORD PTR \[esp\+0x([0-9a-f]+)\]')
store = re.compile(r'mov\s+DWORD PTR \[(\w+)\+0x([0-9a-f]+)\],(\w+)')
store_imm = re.compile(r'mov\s+DWORD PTR \[(\w+)\+0x([0-9a-f]+)\],0x([0-9a-f]+)')


def handler_args(fn, end, rec_reg="esi"):
    """Map argument index -> record dword by parsing the handler's pushes.

    The handlers of the record tables push the record dwords one by one, but
    they interleave *extra* values (string pointers, converted handles) in the
    middle of the list, so the naive "arg = slot/4" arithmetic is off by the
    number of extras pushed after a given dword.  Walking the push sequence is
    exact: arg index = (n_pushes - 1 - position).

    Returns a list indexed by argument number; each item is
    ``("rec", dword_index)``, ``("imm", value)`` or ``None``.
    """
    load_rec = re.compile(r"mov\s+(\w+),DWORD PTR \[" + rec_reg + r"\+0x([0-9a-f]+)\]")
    load_loc = re.compile(r"mov\s+(\w+),DWORD PTR \[esp\+0x([0-9a-f]+)\]")
    load_zx = re.compile(r"movzx\s+(\w+),BYTE PTR \[" + rec_reg + r"\+0x([0-9a-f]+)\]")
    src = {}
    pushes = []
    rows = [(va, line) for va, line in exelib.asm_rows() if fn <= va < end]
    for idx, (va, line) in enumerate(rows):
        ln = line.strip()
        m = load_rec.search(ln) or load_zx.search(ln)
        if m:
            src[m.group(1)] = ("rec", int(m.group(2), 16) // 4)
            continue
        m = load_loc.search(ln)
        if m:
            src[m.group(1)] = ("loc", int(m.group(2), 16))
            continue
        m = re.match(r"push\s+(\w+)", ln)
        if m:
            pushes.append(src.get(m.group(1)))
            continue
        m = re.match(r"push\s+0x([0-9a-f]+)", ln)
        if m:
            pushes.append(("imm", int(m.group(1), 16)))
            continue
        if ln.startswith("call "):
            # arguments of a nested call (cleaned up by the following
            # ``add esp,N``) are not table arguments -- drop them again
            for _, nxt in rows[idx + 1:idx + 3]:
                mm = re.match(r"add\s+esp,0x([0-9a-f]+)", nxt.strip())
                if mm:
                    for _ in range(int(mm.group(1), 16) // 4):
                        if pushes:
                            pushes.pop()
                    break
            continue
        if ln.startswith(("xor ", "mov ", "add ", "sub ", "shl ", "shr ",
                          "or ", "and ", "inc ", "dec ")):
            # a plain register write invalidates the tracked source
            m = re.match(r"(?:xor|mov|add|sub|shl|shr|or|and|inc|dec)\s+(\w+),?",
                         ln)
            if m and m.group(1) in src and not ln.startswith("mov "):
                src.pop(m.group(1), None)
    n = len(pushes)
    args = [None] * n
    for i, p in enumerate(pushes):
        args[n - 1 - i] = p
    return args


def layout(fn, base, end, stride, esp_bias, nrec, args=None):
    win = base + (stride * nrec if stride else 0x4000)
    src = {}
    rows = []
    for va, line in exelib.asm_rows():
        if not (fn <= va < end):
            continue
        m = load.search(line)
        if m:
            src[m.group(1)] = int(m.group(2), 16)
            continue
        m = store.search(line)
        if m:
            addr = int(m.group(2), 16)
            if not (base <= addr < win):
                continue
            if m.group(3) in src:
                slot = src[m.group(3)]
                # arg0 is the last pushed; slot = esp_bias + 4*(nargs-arg)
                arg = (slot - esp_bias - 4) // 4
                what, dw = None, None
                if args is not None and 0 <= arg < len(args) and args[arg]:
                    src_arg = args[arg]
                    if src_arg[0] == "rec":
                        dw = src_arg[1]
                        what = str(dw) if dw < nrec else \
                            "extra arg %d (string/handle)" % arg
                    else:
                        what = "extra arg %d" % arg
                if what is None:
                    what = "%d" % (arg - 1) if arg - 1 < nrec else \
                        "extra arg %d (string/handle)" % arg
                    dw = arg - 1
                rows.append((addr, base, what, dw))
            else:
                rows.append((addr, base, '?', None))
            continue
        m = store_imm.search(line)
        if m:
            addr = int(m.group(2), 16)
            if base <= addr < win:
                rows.append((addr, base, '0x' + m.group(3), None))
    return rows


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('fn', help='store function VMA (hex)')
    ap.add_argument('base', help='record array base VMA (hex)')
    ap.add_argument('--end', default=None, help='end of the function (hex)')
    ap.add_argument('--stride', default=None, help='record stride in bytes')
    ap.add_argument('--esp-bias', type=lambda s: int(s, 0), default=20,
                    help='esp offset of arg0 slot relative to the frame base '
                         '(default 20 = 5 pushes)')
    ap.add_argument('--nrec', type=int, default=95,
                    help='number of record dwords before the extra args')
    ap.add_argument('--handler', default=None,
                    help='handler VA range "LO HI" (e.g. 0x45ccc0 0x45d050) to '
                         'derive the exact arg -> record dword map from the '
                         'push sequence')
    opts = ap.parse_args(argv)

    fn = int(opts.fn, 16)
    base = int(opts.base, 16)
    end = int(opts.end, 16) if opts.end else fn + 0x800
    stride = int(opts.stride, 16) if opts.stride else None

    args = None
    if opts.handler:
        hlo, hhi = [int(x, 16) for x in opts.handler.split()]
        args = handler_args(hlo, hhi)

    rows = layout(fn, base, end, stride, opts.esp_bias, opts.nrec, args)
    seen = {}
    for addr, _b, what, dw in rows:
        off = addr - base
        if off < 0 or (stride and off >= stride):
            print('# outside the record: 0x%x <- %s' % (addr, what))
            continue
        seen[off] = what
    for off in sorted(seen):
        print('mem +0x%03x  <-  record dword %s' % (off, seen[off]))
    print('# %d fields, span +0x000..+0x%03x' % (len(seen), max(seen) if seen else 0))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
