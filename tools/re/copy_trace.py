#!/usr/bin/env python3
"""Trace a block that copies one array record into another array record.

The engine's ``DEFINE_*`` post-processing (e.g. the chassis one at
``0x47ac1a``) unpacks a record with a long run of::

    mov  reg,[edx+0x799748]     ; load  <src array> + off
    mov  [ebx+0x81c4a8],reg     ; store <dst array> + off

sometimes with a scale factor (``lea eax,[eax+eax*4]`` = x5, ``shl eax,2``
= x4) and with inlined ``strlen`` + ``rep movs`` string copies.  The script
tracks the registers and prints every ``src offset -> dst offset`` pair.

Usage:
    python3 copy_trace.py 0x47ac1a 0x47af3e --src 0x7995e0 --stride 0x1cc
    python3 copy_trace.py 0x47ac1a 0x47af3e --src 0x7995e0 --stride 0x1cc --dst 0x81bd00

``--src`` converts absolute source addresses into record offsets (modulo
``--stride``); ``--dst`` is only used to mark the destination base.
"""
import argparse
import re
import sys

import exelib

REG = r"(e[a-ds]x|e[bds]i)"
LOAD = re.compile(r"mov\s+" + REG + r",DWORD PTR \[(\w+)\+0x([0-9a-f]+)\]")
LOADZX = re.compile(r"movzx\s+" + REG + r",BYTE PTR \[(\w+)\+0x([0-9a-f]+)\]")
LEA = re.compile(r"lea\s+" + REG + r",\[(\w+)\+0x([0-9a-f]+)\]")
STORE = re.compile(r"mov\s+(DWORD|WORD|BYTE) PTR \[(\w+)\+0x([0-9a-f]+)\],(\w+)")
SCALE_LEA = re.compile(r"lea\s+" + REG + r",\[(\w+)\+(\w+)\*(\d+)\]")
SHL = re.compile(r"shl\s+" + REG + r",0x([0-9a-f]+)")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("lo")
    ap.add_argument("hi")
    ap.add_argument("--src", required=True, help="source array base, hex")
    ap.add_argument("--src-reg", default="edx", help="register holding the source record (default edx)")
    ap.add_argument("--dst-reg", default="ebx", help="register holding the destination record (default ebx)")
    ap.add_argument("--stride", default=None, help="source record stride, hex")
    ap.add_argument("--dst", default=None, help="destination array base, hex")
    ap.add_argument("--map", dest="mapargs", default=None,
                    help="join with a store function: 'FN BASE END STRIDE' "
                         "(prints the file record dword for each source offset)")
    opts = ap.parse_args(argv)

    lo, hi = int(opts.lo, 16), int(opts.hi, 16)
    src = int(opts.src, 16)
    stride = int(opts.stride, 16) if opts.stride else None
    dst = int(opts.dst, 16) if opts.dst else 0

    reg = {}       # reg -> ("val", off, scale) | ("ptr", off, side)
    out = []
    for va, line in exelib.asm_rows():
        if not (lo <= va < hi):
            continue
        ln = line.strip()

        m = LEA.search(ln)
        if m:
            r, base, off = m.group(1), m.group(2).lower(), int(m.group(3), 16)
            if base == opts.src_reg.lower():
                reg[r] = ("ptr", off, "src")
            elif base in ("ebx", "edi", "esi", "ebp") and off >= dst:
                reg[r] = ("ptr", off, "dst")
            else:
                reg.pop(r, None)
            continue

        m = LOAD.search(ln)
        if m:
            r, base, off = m.group(1), m.group(2).lower(), int(m.group(3), 16)
            if base == opts.src_reg.lower():
                reg[r] = ("val", off, 1)
            else:
                reg.pop(r, None)
            continue

        m = LOADZX.search(ln)
        if m:
            r, base, off = m.group(1), m.group(2).lower(), int(m.group(3), 16)
            if base == opts.src_reg.lower():
                reg[r] = ("val", off, 1)
            else:
                reg.pop(r, None)
            continue

        m = SCALE_LEA.search(ln)
        if m:
            r, a, b, k = m.group(1), m.group(2), m.group(3), int(m.group(4))
            if a == b and r in reg and reg[r][0] == "val":
                _, off, sc = reg[r]
                reg[r] = ("val", off, sc * (k + 1))
            continue

        m = SHL.search(ln)
        if m:
            r, k = m.group(1), int(m.group(2), 16)
            if r in reg and reg[r][0] == "val":
                _, off, sc = reg[r]
                reg[r] = ("val", off, sc * (1 << k))
            continue

        m = STORE.search(ln)
        if m:
            size, base, off, r = m.groups()
            if r in reg:
                kind, soff, sc = reg[r]
                if kind == "val":
                    out.append(("val", soff, int(off, 16), size, sc))
                else:
                    out.append(("ptr", soff, int(off, 16), size, 1))
                del reg[r]
            continue

        if ln.startswith("rep movs"):
            sp = dp = None
            for v in reg.values():
                if v[0] == "ptr":
                    if v[2] == "src":
                        sp = v[1]
                    else:
                        dp = v[1]
            if sp is not None and dp is not None:
                out.append(("str", sp, dp, "ptr", 1))
            continue

    mem2dw = {}
    if opts.mapargs:
        import table_layout
        fn, mbase, mend, mstride = [int(x, 16) for x in opts.mapargs.split()]
        for addr, _b, what, dw in table_layout.layout(
                fn, mbase, mend, mstride, 20, 95):
            if dw is not None:
                mem2dw[addr - mbase] = dw

    for kind, soff, doff, size, sc in out:
        rel = soff - src
        if stride:
            rel %= stride
        scale = "" if sc == 1 else "  x%d" % sc
        target = doff - dst if dst else doff
        dw = mem2dw.get(rel)
        tag = "  (file dword %d)" % dw if dw is not None else ""
        if kind == "val":
            print("src +0x%03x%-8s ->  dst +0x%05x  (%s)%s"
                  % (rel, scale, target, size, tag))
        elif kind == "str":
            print("src +0x%03x          ->  dst +0x%05x  (string)%s"
                  % (rel, target, tag))
        else:
            print("src +0x%03x          ->  dst +0x%05x  (pointer)%s"
                  % (rel, target, tag))
    print("# %d entries" % len(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
