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
store = re.compile(r'mov\s+DWORD PTR \[(\w+)\+0x(79[0-9a-f]{4})\],(\w+)')
store_imm = re.compile(r'mov\s+DWORD PTR \[(\w+)\+0x(79[0-9a-f]{4})\],0x([0-9a-f]+)')


def layout(fn, base, end, stride, esp_bias, nrec):
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
            if m.group(3) in src:
                slot = src[m.group(3)]
                # arg0 is the last pushed; slot = esp_bias + 4*(nargs-arg)
                arg = (slot - esp_bias - 4) // 4
                what = '%d' % (arg - 1) if arg - 1 < nrec else \
                    'extra arg %d (string/handle)' % arg
                rows.append((addr, base, what, arg - 1))
            else:
                rows.append((addr, base, '?', None))
            continue
        m = store_imm.search(line)
        if m:
            rows.append((int(m.group(2), 16), base, '0x' + m.group(3), None))
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
    opts = ap.parse_args(argv)

    fn = int(opts.fn, 16)
    base = int(opts.base, 16)
    end = int(opts.end, 16) if opts.end else fn + 0x800
    stride = int(opts.stride, 16) if opts.stride else None

    rows = layout(fn, base, end, stride, opts.esp_bias, opts.nrec)
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
