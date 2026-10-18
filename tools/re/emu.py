#!/usr/bin/env python3
"""Minimal x86 interpreter driven by objdump's disassembly text.

Purpose: faithfully execute gundx.exe's SBV frame decoder
(0x49ee8c + 0x49f4db / 0x49f1e3 / 0x49f43a / 0xa1d000 / 0xa1d940)
so the custom video codec can be reproduced without hand-translating it.

Needs ``objdump`` and the (read-only) game executable; the path comes from
``GUNMETAL_EXE`` or the default below.  Usage example::

    import emu
    emu.load_image()
    emu.R['esp'] = 0x9fc000
    emu.push(emu.SENTINEL)
    emu.run(0x49ee8c)
    print(emu.rd(0x4bee20, 4))
"""
import os, subprocess, re, struct, sys

EXE = os.environ.get('GUNMETAL_EXE', '/mnt/d/games/Gunmetal 3dfx/gundx.exe')
IMG_BASE = 0x400000
MEM_HI = 0xa20000
mem = bytearray(MEM_HI - IMG_BASE)

# ---------------------------------------------------------------- memory
def rd(a, n):
    o = a - IMG_BASE
    return int.from_bytes(mem[o:o + n], 'little')

def wr(a, n, v):
    o = a - IMG_BASE
    mem[o:o + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')

def load_image():
    d = open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    off = pe + 24 + opt
    for _ in range(nsec):
        nm = d[off:off + 8].rstrip(b'\x00').decode('latin1')
        vsz, vma, rsz, raw = struct.unpack_from('<IIII', d, off + 8)
        if rsz and vma >= 0x1000:
            va = IMG_BASE + vma
            blob = d[raw:raw + min(rsz, vsz if vsz else rsz)]
            o = va - IMG_BASE
            mem[o:o + len(blob)] = blob
        off += 40

# ------------------------------------------------------------- registers
R = {'eax': 0, 'ebx': 0, 'ecx': 0, 'edx': 0,
     'esi': 0, 'edi': 0, 'ebp': 0, 'esp': 0}
R32 = list(R)
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx',
       'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
F = {'cf': 0, 'zf': 0, 'sf': 0, 'of': 0}

def get_reg(n):
    if n in R32:
        return R[n]
    if n in R8L:
        return R[R8L[n]] & 0xff
    if n in R8H:
        return (R[R8H[n]] >> 8) & 0xff
    if n in R16:
        return R[R16[n]] & 0xffff
    if n == 'eip':
        return 0
    raise KeyError(n)

def set_reg(n, v):
    if n in R32:
        R[n] = v & 0xffffffff
    elif n in R8L:
        r = R8L[n]; R[r] = (R[r] & ~0xff) | (v & 0xff)
    elif n in R8H:
        r = R8H[n]; R[r] = (R[r] & ~0xff00) | ((v & 0xff) << 8)
    elif n in R16:
        r = R16[n]; R[r] = (R[r] & ~0xffff) | (v & 0xffff)
    else:
        raise KeyError(n)

def regsize(n):
    if n in R32: return 4
    if n in R8L or n in R8H: return 1
    if n in R16: return 2
    raise KeyError(n)

# ------------------------------------------------------------ disassembly
_CACHE = {}
_LINE = re.compile(r'^\s+([0-9a-f]+):\t([^\t]*)(?:\t(.*))?$')

def dis(a):
    if a in _CACHE:
        return _CACHE[a]
    out = subprocess.run(['objdump', '-D', '-M', 'intel',
                          '--start-address=%#x' % a,
                          '--stop-address=%#x' % (a + 16), EXE],
                         capture_output=True, text=True).stdout
    cur = None
    ins = []
    for line in out.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        ad = int(m.group(1), 16)
        by = m.group(2).strip()
        tx = m.group(3)
        if tx is None or tx.strip() == '':
            if cur is not None:
                cur[1] += ' ' + by
                cur[3] = len(cur[1].split())
            continue
        cur = [ad, by, tx.strip(), len(by.split())]
        ins.append(cur)
    first = None
    for x in ins:
        if x[0] == a:
            first = x
            break
    if first is None:
        raise RuntimeError('no disasm at %#x' % a)
    _CACHE[a] = (first[2], first[3])
    return first[2], first[3]

# ------------------------------------------------------------- operands
SEGS = ('ds', 'es', 'cs', 'ss', 'fs', 'gs')

def split_ops(text):
    parts = text.split(None, 1)
    mn = parts[0]
    rest = parts[1].strip() if len(parts) > 1 else ''
    if not rest:
        return mn, []
    return mn, [o.strip() for o in rest.split(',')]

def strip_size(s):
    m = re.match(r'^(byte|word|dword|qword)\s+ptr\s+(.*)$', s, re.I)
    if m:
        return {'byte': 1, 'word': 2, 'dword': 4, 'qword': 8}[m.group(1).lower()], m.group(2).strip()
    return None, s

def mem_addr(expr):
    e = expr.strip()
    if ':' in e and e.split(':')[0] in SEGS:
        e = e.split(':', 1)[1]
    if e.startswith('['):
        e = e[1:-1]
    base = 0
    idx = 0
    for t in re.findall(r'[+-]?[^+-]+', e.replace(' ', '')):
        sign = 1
        if t[0] == '+':
            t = t[1:]
        elif t[0] == '-':
            sign = -1; t = t[1:]
        if '*' in t:
            r, k = t.split('*')
            idx += sign * get_reg(r) * int(k, 0)
        elif t in R32:
            base += sign * get_reg(t)
        else:
            base += sign * (int(t, 16) if t.lower().startswith('0x') else int(t, 10))
    return (base + idx) & 0xffffffff

def op_size(op):
    sz, body = strip_size(op)
    return sz

def eval_op(op, size):
    """returns ('r',name) | ('i',val) | ('m',addr)"""
    sz, body = strip_size(op)
    if body in R32 or body in R8L or body in R8H or body in R16:
        return ('r', body)
    b = body.strip()
    neg = False
    if b.startswith('-'):
        neg = True; b = b[1:]
    if b.lower().startswith('0x') or b.isdigit():
        v = int(b, 16) if b.lower().startswith('0x') else int(b, 10)
        return ('i', -v if neg else v)
    if b == 'eip':
        return ('i', 0)
    if ':' in b and b.split(':')[0] in SEGS or b.startswith('['):
        return ('m', mem_addr(b))
    if re.match(r'^[0-9a-f]+$', b):  # bare hex without 0x
        return ('i', int(b, 16))
    raise RuntimeError('cannot eval operand %r' % op)

def getv(op, size):
    k = eval_op(op, size)
    if k[0] == 'r':
        return get_reg(k[1])
    if k[0] == 'i':
        return k[1] & ((1 << (8 * size)) - 1) if size else k[1]
    return rd(k[1], size)

def setv(op, size, v):
    k = eval_op(op, size)
    if k[0] == 'r':
        set_reg(k[1], v)
    elif k[0] == 'm':
        wr(k[1], size, v)
    else:
        raise RuntimeError('cannot write operand %r' % op)

# ---------------------------------------------------------------- flags
def _sz(sz):
    return 1 << (8 * sz)

def flags_arith(a, b, res, sz, op):
    m = _sz(sz); sgn = m >> 1
    F['zf'] = 1 if (res & (m - 1)) == 0 else 0
    F['sf'] = 1 if (res & sgn) else 0
    if op == 'add':
        F['cf'] = 1 if res >= m else 0
        F['of'] = 1 if ((a ^ res) & (b ^ res) & sgn) else 0
    elif op == 'sub':
        F['cf'] = 1 if (a & (m - 1)) < (b & (m - 1)) else 0
        F['of'] = 1 if ((a ^ b) & (a ^ res) & sgn) else 0

def do_add(a, b, sz):
    m = _sz(sz); res = a + b
    flags_arith(a, b, res, sz, 'add')
    return res & (m - 1)

def do_sub(a, b, sz):
    m = _sz(sz); res = a - b
    flags_arith(a, b, res, sz, 'sub')
    return res & (m - 1)

def do_logic(res, sz):
    m = _sz(sz)
    F['cf'] = 0; F['of'] = 0
    F['zf'] = 1 if (res & (m - 1)) == 0 else 0
    F['sf'] = 1 if (res & (m >> 1)) else 0
    return res & (m - 1)

def do_shift(kind, a, c, sz):
    m = _sz(sz); bits = 8 * sz
    a &= m - 1
    if c == 0:
        return a
    if kind == 'shl':
        r = (a << c) & (m - 1)
        F['cf'] = (a >> (bits - c)) & 1
        F['of'] = F['cf'] ^ (1 if (r & (m >> 1)) else 0)
    elif kind == 'shr':
        F['cf'] = (a >> (c - 1)) & 1
        r = (a >> c) & (m - 1)
        F['of'] = 1 if (c == 1 and (a & (m >> 1))) else 0
    else:  # sar
        if a & (m >> 1):
            a -= m
        r = (a >> c) & (m - 1)
        F['cf'] = ((a >> (c - 1)) & 1) if c <= bits else 0
        F['of'] = 0
    F['zf'] = 1 if r == 0 else 0
    F['sf'] = 1 if (r & (m >> 1)) else 0
    return r

def cond(mn):
    cf, zf, sf, of = F['cf'], F['zf'], F['sf'], F['of']
    return {
        'je': zf, 'jz': zf, 'jne': not zf, 'jnz': not zf,
        'jb': cf, 'jc': cf, 'jnae': cf, 'jae': not cf, 'jnb': not cf, 'jnc': not cf,
        'jbe': cf or zf, 'jna': cf or zf, 'ja': not cf and not zf, 'jnbe': not cf and not zf,
        'jl': sf != of, 'jnge': sf != of, 'jge': sf == of, 'jnl': sf == of,
        'jle': zf or (sf != of), 'jng': zf or (sf != of),
        'jg': not zf and (sf == of), 'jnle': not zf and (sf == of),
        'js': sf, 'jns': not sf, 'jo': of, 'jno': not of,
    }[mn]

# ------------------------------------------------------------------ x87
ST = []

def f_rd(op):
    sz, body = strip_size(op)
    k = eval_op(body, 8)
    if k[0] == 'i':
        return struct.unpack('<d', (k[1] & ((1 << 64) - 1)).to_bytes(8, 'little'))[0]
    return struct.unpack('<d', mem[k[1] - IMG_BASE:k[1] - IMG_BASE + 8])[0]

# ----------------------------------------------------------------- hooks
def hook_float_to_int():
    """0x40e0e0: (int64)st0 with truncation, returns eax=lo, edx=hi."""
    v = ST.pop()
    i = int(v)  # truncate toward zero
    i &= (1 << 64) - 1
    R['eax'] = i & 0xffffffff
    R['edx'] = (i >> 32) & 0xffffffff

HOOKS = {0x40e0e0: hook_float_to_int}

# ------------------------------------------------------------- execution
def push(v):
    R['esp'] = (R['esp'] - 4) & 0xffffffff
    wr(R['esp'], 4, v)

def pop():
    v = rd(R['esp'], 4)
    R['esp'] = (R['esp'] + 4) & 0xffffffff
    return v

def to_signed(v, sz):
    m = 1 << (8 * sz)
    return v - m if v & (m >> 1) else v

SENTINEL = 0x5ffffff0
HIST = []
COLLECT = None   # set to (lo,hi) to record executed instructions
WATCH = {}       # addr -> list of register snapshots
WATCHFN = {}     # addr -> callable() invoked before executing that address
TRACE = None     # [(lo, hi), ...] -> append (eip, text, dict(R)) for any range

def run(start, maxsteps=200_000_000, verbose=False):
    eip = start
    steps = 0
    while True:
        steps += 1
        if eip == SENTINEL:
            return steps
        if steps > maxsteps:
            raise RuntimeError('step limit at %#x' % eip)
        try:
            return _run_body(eip, steps, verbose, maxsteps)
        except Exception:
            for h in HIST[-60:]:
                print('  %08x  %s' % h, file=sys.stderr)
            raise


def _run_body(eip, steps, verbose, maxsteps):
    while True:
        steps += 1
        if eip == SENTINEL:
            return steps
        if steps > maxsteps:
            raise RuntimeError('step limit at %#x' % eip)
        text, nb = dis(eip)
        mn, ops = split_ops(text)
        nxt = eip + nb
        HIST.append((eip, text))
        if len(HIST) > 400:
            del HIST[:200]
        if COLLECT is not None and COLLECT[0] <= eip < COLLECT[1]:
            COLLECT[2][eip] = text
        if eip in WATCH:
            WATCH[eip].append(dict(R))
        if eip in WATCHFN:
            WATCHFN[eip]()
        if TRACE is not None:
            for _lo, _hi, _lst in TRACE:
                if _lo <= eip < _hi:
                    _lst.append((eip, text, dict(R)))
                    break
        if verbose:
            print('%08x  %s' % (eip, text))
        # ---- multi-mnemonic lines
        if mn == 'rep':
            m2, o2 = split_ops(text.split(None, 1)[1])
            cnt = R['ecx']
            if m2 in ('stos',):
                sz, body = strip_size(o2[0])
                if sz is None: sz, body = strip_size(ops[0])
                val = getv(o2[-1], sz) if len(o2) > 1 else R['eax']
                for _ in range(cnt):
                    wr(R['edi'], sz, val)
                    R['edi'] += sz
            elif m2 in ('movs',):
                sz, body = strip_size(o2[0])
                if sz is None: sz = 4
                for _ in range(cnt):
                    wr(R['edi'], sz, rd(R['esi'], sz))
                    R['esi'] += sz; R['edi'] += sz
            elif m2 in ('sto',):
                pass
            else:
                raise RuntimeError('REP %s at %#x' % (m2, eip))
            R['ecx'] = 0
            eip = nxt
            continue
        # ---- mov family
        if mn in ('mov',):
            s = None
            if eval_op(ops[0], 4)[0] == 'r':
                s = regsize(eval_op(ops[0], 4)[1])
            if s is None:
                s = op_size(ops[0]) or op_size(ops[1]) or 4
            setv(ops[0], s, getv(ops[1], s))
        elif mn == 'movzx':
            s = op_size(ops[1]) or 1
            d = regsize(eval_op(ops[0], 4)[1])
            setv(ops[0], d, getv(ops[1], s) & ((1 << (8 * s)) - 1))
        elif mn == 'movsx':
            s = op_size(ops[1]) or 1
            d = regsize(eval_op(ops[0], 4)[1])
            setv(ops[0], d, to_signed(getv(ops[1], s), s) & ((1 << (8 * d)) - 1))
        elif mn == 'lea':
            a = mem_addr(ops[1])
            setv(ops[0], 4, a)
        # ---- arithmetic
        elif mn in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'adc', 'sbb'):
            s = op_size(ops[0]) or (regsize(eval_op(ops[0], 4)[1])
                                    if eval_op(ops[0], 4)[0] == 'r' else 4)
            a = getv(ops[0], s); b = getv(ops[1], s)
            if mn == 'add':
                r = do_add(a, b, s)
            elif mn == 'adc':
                r = do_add(a, b + F['cf'], s)
            elif mn == 'sub':
                r = do_sub(a, b, s)
            elif mn == 'sbb':
                r = do_sub(a, b + F['cf'], s)
            elif mn == 'cmp':
                do_sub(a, b, s); r = None
            elif mn == 'and':
                r = do_logic(a & b, s)
            elif mn == 'or':
                r = do_logic(a | b, s)
            elif mn == 'xor':
                r = do_logic(a ^ b, s)
            elif mn == 'test':
                do_logic(a & b, s); r = None
            if r is not None:
                setv(ops[0], s, r)
        elif mn in ('inc', 'dec'):
            s = op_size(ops[0]) or (regsize(eval_op(ops[0], 4)[1])
                                    if eval_op(ops[0], 4)[0] == 'r' else 4)
            a = getv(ops[0], s)
            cf = F['cf']
            r = do_add(a, 1, s) if mn == 'inc' else do_sub(a, 1, s)
            F['cf'] = cf
            setv(ops[0], s, r)
        elif mn == 'neg':
            s = op_size(ops[0]) or 4
            a = getv(ops[0], s)
            r = do_sub(0, a, s)
            setv(ops[0], s, r)
        elif mn == 'not':
            s = op_size(ops[0]) or 4
            setv(ops[0], s, (~getv(ops[0], s)))
        elif mn in ('shl', 'sal', 'shr', 'sar'):
            s = op_size(ops[0]) or (regsize(eval_op(ops[0], 4)[1])
                                    if eval_op(ops[0], 4)[0] == 'r' else 4)
            a = getv(ops[0], s)
            c = get_reg('ecx') & 31 if ops[1] == 'cl' else int(ops[1], 0)
            k = 'shl' if mn == 'sal' else mn
            setv(ops[0], s, do_shift(k, a, c & 31, s))
        elif mn == 'imul':
            if len(ops) == 3:
                s = op_size(ops[1]) or 4
                d = regsize(eval_op(ops[0], 4)[1])
                a = to_signed(getv(ops[1], s), s); b = to_signed(getv(ops[2], s), s)
                setv(ops[0], d, (a * b) & ((1 << (8 * d)) - 1))
            elif len(ops) == 2:
                s = op_size(ops[1]) or (regsize(eval_op(ops[1], 4)[1])
                                        if eval_op(ops[1], 4)[0] == 'r' else 4)
                a = to_signed(getv(ops[0], s), s); b = to_signed(getv(ops[1], s), s)
                setv(ops[0], s, (a * b) & ((1 << (8 * s)) - 1))
            else:
                s = op_size(ops[0]) or 4
                a = to_signed(R['eax'], s); b = to_signed(getv(ops[0], s), s)
                r = a * b
                if s == 4:
                    R['eax'] = r & 0xffffffff; R['edx'] = (r >> 32) & 0xffffffff
                else:
                    R['eax'] = r & 0xffff
        elif mn == 'cdq':
            R['edx'] = 0xffffffff if (R['eax'] & 0x80000000) else 0
        elif mn in ('idiv', 'div'):
            s = op_size(ops[0]) or (regsize(eval_op(ops[0], 4)[1])
                                    if eval_op(ops[0], 4)[0] == 'r' else 4)
            if mn == 'idiv':
                d = to_signed(getv(ops[0], s), s)
                n = to_signed(R['eax'], s)
                q = int(n / d); rm = n - q * d
            else:
                d = getv(ops[0], s)
                n = R['eax']
                q = n // d; rm = n % d
            if s == 4:
                R['eax'] = q & 0xffffffff; R['edx'] = rm & 0xffffffff
            else:
                R['eax'] = ((rm & 0xffff) << 16) | (q & 0xffff)
        # ---- stack / control
        elif mn == 'push':
            k = op_size(ops[0])
            if ops[0] in R32:
                push(R[ops[0]])
            elif ops[0].startswith('0x') or ops[0].lstrip('-').isdigit():
                push(int(ops[0], 16) if ops[0].startswith('0x') else int(ops[0], 10))
            else:
                push(getv(ops[0], k or 4))
        elif mn == 'pop':
            setv(ops[0], 4, pop())
        elif mn == 'leave':
            R['esp'] = R['ebp']; R['ebp'] = pop()
        elif mn == 'xchg':
            a = eval_op(ops[0], 4); b = eval_op(ops[1], 4)
            if a[0] == 'r' and b[0] == 'r':
                va = get_reg(a[1]); vb = get_reg(b[1])
                set_reg(a[1], vb); set_reg(b[1], va)
            else:
                raise RuntimeError('xchg mem unsupported')
        elif mn == 'nop':
            pass
        elif mn == 'int3':
            raise RuntimeError('int3 at %#x' % eip)
        elif mn == 'call':
            tgt = ops[0]
            if tgt.startswith('0x'):
                t = int(tgt, 16)
                if t in HOOKS:
                    HOOKS[t]()
                    eip = nxt
                    continue
                push(nxt)
                eip = t
                continue
            else:
                k = eval_op(tgt, 4)
                t = get_reg(k[1]) if k[0] == 'r' else rd(k[1], 4)
                push(nxt); eip = t; continue
        elif mn == 'jmp':
            tgt = ops[0]
            if tgt.startswith('0x'):
                eip = int(tgt, 16); continue
            k = eval_op(tgt, 4)
            eip = get_reg(k[1]) if k[0] == 'r' else rd(k[1], 4)
            continue
        elif mn == 'ret':
            eip = pop()
            if len(ops) == 1:
                R['esp'] = (R['esp'] + int(ops[0], 0)) & 0xffffffff
            continue
        elif mn.startswith('j') and mn not in ('jmp',) and mn in (
                'je', 'jz', 'jne', 'jnz', 'jb', 'jc', 'jnae', 'jae', 'jnb', 'jnc',
                'jbe', 'jna', 'ja', 'jnbe', 'jl', 'jnge', 'jge', 'jnl', 'jle',
                'jng', 'jg', 'jnle', 'js', 'jns', 'jo', 'jno'):
            if cond(mn):
                eip = int(ops[0], 16)
            else:
                eip = nxt
            continue
        # ---- x87
        elif mn == 'fild':
            v = to_signed(getv(ops[0], op_size(ops[0]) or 4), op_size(ops[0]) or 4)
            ST.append(float(v))
        elif mn == 'fld':
            ST.append(f_rd(ops[0]))
        elif mn == 'fmul':
            ST[-1] = ST[-1] * f_rd(ops[0])
        elif mn == 'fdiv':
            ST[-1] = ST[-1] / f_rd(ops[0])
        elif mn == 'fiadd':
            v = to_signed(getv(ops[0], op_size(ops[0]) or 4), op_size(ops[0]) or 4)
            ST[-1] = ST[-1] + v
        elif mn == 'fsub':
            ST[-1] = ST[-1] - f_rd(ops[0])
        elif mn == 'fadd':
            ST[-1] = ST[-1] + f_rd(ops[0])
        elif mn == 'fstp':
            sz, body = strip_size(ops[0])
            if sz == 4:
                setv(body, 4, struct.unpack('<I', struct.pack('<f', ST[-1]))[0])
            else:
                wr(mem_addr(body), 8, int.from_bytes(struct.pack('<d', ST[-1]), 'little'))
            ST.pop()
        elif mn == 'fistp':
            sz, body = strip_size(ops[0])
            i = int(ST[-1]) & ((1 << (8 * sz)) - 1)
            wr(mem_addr(body), sz, i)
            ST.pop()
        elif mn in ('fldcw', 'fstcw', 'fwait', 'fnstcw', 'fclex', 'finit'):
            pass
        else:
            raise RuntimeError('unimplemented mnemonic %r at %#x' % (mn, eip))
        eip = nxt


# ------------------------------------------------------------------ main
if __name__ == '__main__':
    load_image()
    print('image loaded; type something to run is driven by caller')
