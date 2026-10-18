"""Gunmetal ".sbv" video clips (Vid/*.sbv).

Container
---------
Verified against ``gundx.exe``'s reader at VMA 0x49e1e8 (the width is rejected
if it exceeds 0x140 = 320)::

    char magic[4];      // "SBV0"
    u32  width;         // <= 320
    u32  height;
    u32  frame_count;
    frame_count * {
        u32 timestamp_ms;   // playback stamp (~1000/15 ms step)
        u32 end_offset;     // cumulative end offset, relative to data_start
    };

``data_start = 16 + 8 * frame_count``.  Frame *i* occupies the byte range
``[data_start + prev, data_start + end[i])`` with ``prev = end[i-1]`` (0 for
i = 0); the last ``end`` lands exactly on EOF for all three shipped files.

Payload codec
-------------
A custom, purely intra-coded **pyramidal** scheme (no Bink/Smacker/Indeo).
Every frame holds three planes -- one luma (W x H) and two chroma
(W/2 x H/2) -- and each plane is reconstructed bottom-up:

1. the source pointer starts at two signed 32-bit bounds ``min``/``max``,
   followed by one byte per sample of the coarsest ``(W/8)x(H/8)`` grid
   (for chroma) or ``(W/4)x(H/4)`` grid (for luma).  A base sample is
   ``(int)(byte * (max - min) / 256.0 + min)`` (fp division by the double
   256.0 at 0x4a2ac0, truncated toward zero);
2. a bit-level entropy stream then fills the remaining samples: each step
   decodes a run of ``factor x factor`` blocks (``factor`` 4 -> 8) that are
   scattered into the plane, and an inverse lifting / gap-filling pair
   (0xa1d940 + 0x49f1e3 + 0x49f43a) closes the gaps left between blocks.

(The ``min + byte * (max - min) / 256.0`` step is evaluated in IEEE double here;
the engine uses x87 extended precision.  The two can only disagree when the
product lands within a few 1e-15 of an integer, which no shipped sample does.)

Entropy decoder (0xa1d000)
--------------------------
MSB-first bit reader over the frame bytes.  Per block: a signed DPCM-coded
unary *class*; class 0 fills the whole block with zeros and class >= 0x11 ends
the plane.  Otherwise a signed DPCM-coded unary *step exponent* (``step =
1 << (e & 31)``) and a per-block sign bit follow, and one of 17 static handlers
runs.  Classes 1 and 3 are the only ones used by shipped content:

* class 1 -- per slot: when the cursor is 16-byte aligned and the next bit is 0
  four zeros are emitted and the cursor advances by 16 bytes; otherwise a
  ``nonzero?`` bit and, when set, a sign bit give ``+/- step``;
* class 3 -- like class 1, but the value is ``(1 + number of following 1-bits) *
  step``, the sign bit being read *before* the run.

Classes 2 and 4..16 use the "family" handlers at ``0xa1d170 + 0x80k``.  They do
not occur in any shipped ``.sbv`` -- decoding every frame of all three files
(6843 frames, 12.3M blocks) yields the class histogram ``{0: 6526044,
1: 3001203, 3: 2790153}``, and ``FAMILY_BLOCKS`` stays 0 -- and the reference
handlers are self-inconsistent (they read bits from the accumulator without
shifting it), so they are handled best-effort here.

Colour conversion (0x49e33b)
----------------------------
Each plane sample is a signed 16-bit value.  With ``Y`` the luma sample and
``C0``/``C1`` the two chroma samples at ``(x >> 1, y >> 1)``::

    r = clamp((Y + C0      + 8) >> 4)
    g = clamp((Y - C0 - C1 + 8) >> 4)
    b = clamp((Y + C1      + 8) >> 4)

The engine feeds those three bytes through run-time built YUV->RGB tables
(0x4a6a40/0x4a6e40/0x4a7240) into a DirectDraw surface; the tables are built in
uninitialised memory, so we go straight to 8-bit RGB.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"SBV0"

# --- diagnostics ----------------------------------------------------------
# Number of blocks decoded through a "family" handler (class 2 or 4..16).  Zero
# for all shipped videos; a non-zero value means the frame used a code path that
# is only approximated here.
FAMILY_BLOCKS = 0


@dataclass
class Frame:
    index: int
    timestamp_ms: int
    offset: int
    data: bytes


@dataclass
class Sbv:
    path: str
    width: int
    height: int
    frame_count: int
    data_start: int
    table: list[tuple[int, int]] = field(default_factory=list)
    file_size: int = 0

    def frames(self, blob: bytes) -> list[Frame]:
        out: list[Frame] = []
        prev = 0
        for i, (ts, end) in enumerate(self.table):
            start = self.data_start + prev
            stop = self.data_start + end
            out.append(Frame(i, ts, start, blob[start:stop]))
            prev = end
        return out

    def frame(self, blob: bytes, index: int) -> Frame:
        if not 0 <= index < self.frame_count:
            raise IndexError(f"frame {index} out of range (0..{self.frame_count - 1})")
        ts, end = self.table[index]
        prev = self.table[index - 1][1] if index else 0
        start = self.data_start + prev
        return Frame(index, ts, start, blob[start:self.data_start + end])


def parse_file(path: str) -> tuple[Sbv, bytes]:
    with open(path, "rb") as f:
        blob = f.read()
    return parse(blob, path), blob


def parse(blob: bytes, path: str = "<mem>") -> Sbv:
    if len(blob) < 16 or blob[:4] != MAGIC:
        raise ValueError(f"not an SBV0 file (magic {blob[:4]!r})")
    width, height, count = struct.unpack_from("<III", blob, 4)
    if width > 0x140:
        raise ValueError(f"implausible width {width} (> 320)")
    need = 16 + 8 * count
    if len(blob) < need:
        raise ValueError(f"table truncated: want {need}, have {len(blob)}")
    table = [struct.unpack_from("<II", blob, 16 + 8 * i) for i in range(count)]
    return Sbv(path, width, height, count, need, table, len(blob))


def validate(sbv: Sbv) -> bool:
    """True when the cumulative offsets tile the file exactly."""
    if not sbv.table:
        return sbv.data_start == sbv.file_size
    return sbv.data_start + sbv.table[-1][1] == sbv.file_size


# =========================================================================
# Plane decoder -- 1:1 transliteration of gundx.exe
#   0x49ee8c  frame/planes        -> _decode_planes
#   0x49f4db  block scatter       -> _scatter_blocks
#   0x49f1e3  lift + edge fixup   -> _filt_edges
#   0x49f43a  gap spread          -> _spread
#   0xa1d940  separable lifting   -> _lift_2d
#   0xa1d000  entropy block       -> _vidseg
# =========================================================================

PLANE_BASE = 0x8c8000          # scratch image base used by the engine
_SCRATCH_WORDS = 256           # entropy scratch buffer (0xa1c000..0xa1c400)


def plane_buffer_size(width: int, height: int) -> int:
    """Bytes needed for luma + two subsampled chroma planes and their gaps."""
    cw, ch = width // 2, height // 2
    return width * height * 2 + 0x20 + 2 * (cw * ch * 2 + 0x20)


class _Mem:
    """u16 plane samples addressed with engine virtual addresses."""

    def __init__(self, size: int):
        self.m = bytearray(size)

    def g16(self, a: int) -> int:
        o = a - PLANE_BASE
        return int.from_bytes(self.m[o:o + 2], "little")

    def p16(self, a: int, v: int) -> None:
        o = a - PLANE_BASE
        self.m[o:o + 2] = (v & 0xFFFF).to_bytes(2, "little")


def _sar16(v: int, n: int) -> int:
    v &= 0xFFFF
    if v & 0x8000:
        v -= 0x10000
    return (v >> n) & 0xFFFF


def _s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def _s32(v: int) -> int:
    return v - 0x100000000 if v & 0x80000000 else v


def _lift_2d(M: _Mem, p: int, W: int, H: int) -> None:
    """0xa1d940: separable inverse lifting over the Hi-res plane."""
    edi = p
    edx = 2 * W
    outer = H - 2
    esi = edx
    ecx = edi + 2 * edx
    while True:
        while True:
            bx = M.g16(edi + edx)
            ax = _sar16(M.g16(ecx + edx) + bx + 2, 2)
            bp = (M.g16(ecx) - ax) & 0xFFFF
            M.p16(ecx, bp)
            ax = _sar16(M.g16(edi) + bp + 1, 1)
            M.p16(edi + edx, (ax + bx) & 0xFFFF)
            edi += 2
            ecx += 2
            esi -= 2
            if esi == 0:
                break
        ecx = edi
        esi = edx
        edi -= edx
        while True:
            edi += 4
            ecx += 4
            esi -= 4
            if esi == 0:
                break
            for base in (edi, ecx):
                ax = _sar16(M.g16(base - 2) + M.g16(base + 2) + 2, 2)
                bp = (M.g16(base) - ax) & 0xFFFF
                ax = _sar16(M.g16(base - 4) + bp + 1, 1)
                M.p16(base, bp)
                M.p16(base - 2, (M.g16(base - 2) + ax) & 0xFFFF)
        ax = M.g16(edi - 4)
        bx = M.g16(ecx - 4)
        M.p16(edi - 2, (M.g16(edi - 2) + ax) & 0xFFFF)
        M.p16(ecx - 2, (M.g16(ecx - 2) + bx) & 0xFFFF)
        edi = ecx
        esi = edx
        outer -= 2
        ecx = edi + 2 * edx
        if outer == 0:
            break


def _filt_edges(M: _Mem, p: int, W: int, H: int) -> None:
    """0x49f1e3: lifting followed by the bottom-row / right-edge fix-ups."""
    _lift_2d(M, p, W, H)
    wh = W * H
    c = wh - W
    d = wh - 2 * W
    for i in range(W):
        a = p + (c + i) * 2
        M.p16(a, (M.g16(a) + M.g16(p + (d + i) * 2)) & 0xFFFF)
    row_b = wh - 2 * W
    row_c = wh - W
    for i in range(2, W, 2):
        for R in (row_b, row_c):
            t = p + (R + i) * 2
            ax = _sar16(M.g16(t - 2) + M.g16(t + 2) + 2, 2)
            bp = (M.g16(t) - ax) & 0xFFFF
            ax = _sar16(M.g16(t - 4) + bp + 1, 1)
            M.p16(t, bp)
            M.p16(t - 2, (M.g16(t - 2) + ax) & 0xFFFF)
    i = W - 1
    for R in (row_b, row_c):
        t = p + (R + i) * 2
        M.p16(t, (M.g16(t) + M.g16(t - 2)) & 0xFFFF)


def _spread(M: _Mem, p: int, W: int, H: int) -> None:
    """0x49f43a: stretch W samples per row backwards, doubling the stride."""
    s = p + (W * H) * 2 - 2
    dd = p + (W * H * 4 - 2 * W) * 2 - 4
    for _ in range(H - 1, -1, -1):
        for _ in range(W - 1, -1, -1):
            M.p16(dd, M.g16(s))
            s -= 2
            dd -= 4
        dd -= W * 4


class _BitState:
    """Decoder state: the engine's 0x6c08e0 struct plus the bit reader."""

    __slots__ = ("src", "pos", "bc", "acc", "pred1", "pred2", "sign", "scratch")

    def __init__(self) -> None:
        self.src = b""
        self.pos = 0
        self.bc = 0
        self.acc = 0
        self.pred1 = 0
        self.pred2 = 0
        self.sign = 0
        self.scratch = [0] * _SCRATCH_WORDS


def _vidseg(st: _BitState, n: int) -> list[int]:
    """0xa1d000: decode one ``n``-dword block.  Returns the n dwords."""
    src = st.src
    pos = st.pos
    bc = st.bc
    acc = st.acc
    scratch = st.scratch
    base = _SCRATCH_WORDS - n

    def bit() -> int:
        nonlocal bc, acc, pos
        bc -= 1
        if bc < 0:
            acc = int.from_bytes(src[pos:pos + 4].ljust(4, b"\x00"), "little")
            pos += 4
            bc = 31
        b = (acc >> 31) & 1
        acc = (acc << 1) & 0xFFFFFFFF
        return b

    def unary() -> int:
        c = 0
        while bit():
            c += 1
        return c

    def setat(idx: int, v: int) -> None:
        if 0 <= idx < _SCRATCH_WORDS:
            scratch[idx] = v & 0xFFFFFFFF

    c = unary()
    if c and bit():
        c = -c
    c = (c + st.pred1) & 0xFFFFFFFF
    st.pred1 = _s32(c)
    if c == 0:
        for k in range(base, _SCRATCH_WORDS):
            scratch[k] = 0
        st.src, st.pos, st.bc, st.acc = src, pos, bc, acc
        return scratch[base:]

    e = unary()
    if e and bit():
        e = -e
    e = (e + st.pred2) & 0xFFFFFFFF
    st.pred2 = _s32(e)
    step = (1 << (e & 31)) & 0xFFFFFFFF
    st.sign = -1 if bit() else 0
    if c >= 0x11:
        st.src, st.pos, st.bc, st.acc = src, pos, bc, acc
        return scratch[base:]

    cls = c
    cur = -4 * n
    if cls == 1 or cls == 3:
        while cur < 0:
            idx = _SCRATCH_WORDS + (cur >> 2)
            if (cur & 0xF) == 0 and bit() == 0:
                setat(idx, 0)
                setat(idx + 1, 0)
                setat(idx + 2, 0)
                setat(idx + 3, 0)
                cur += 0x10
                continue
            val = 0
            if bit():
                if cls == 1:
                    val = step
                    if bit():
                        val = (-val) & 0xFFFFFFFF
                else:
                    s = bit()
                    val = step
                    while bit():
                        val = (val + step) & 0xFFFFFFFF
                    if s:
                        val = (-val) & 0xFFFFFFFF
            setat(idx, val)
            cur += 4
    else:
        global FAMILY_BLOCKS
        FAMILY_BLOCKS += 1
        nb = 2 if cls == 2 else (cls - 1)
        while cur < 0:
            idx = _SCRATCH_WORDS + (cur >> 2)
            v = 0
            for _ in range(nb):
                v = ((v << 1) | bit()) & 0xFFFFFFFF
            if v:
                if v & (1 << (nb - 1)):
                    v -= 1 << nb
                setat(idx, v & 0xFFFFFFFF)
                cur += 4
            else:
                setat(idx, 0)
                setat(idx + 1, 0)
                setat(idx + 2, 0)
                setat(idx + 3, 0)
                cur += 0x10
    st.src, st.pos, st.bc, st.acc = src, pos, bc, acc
    return scratch[base:]


def _scatter_blocks(M: _Mem, W: int, H: int, factor: int, st: _BitState,
                    plane: int) -> None:
    """0x49f4db: place ``factor x factor`` blocks of decoded samples."""
    size = (factor * factor * 3) >> 2
    st.pred1 = 0
    st.pred2 = 0
    for i in range(0, H, factor):
        for j in range(0, W, factor):
            vals = _vidseg(st, size)
            si = 0

            def gv() -> int:
                nonlocal si
                v = vals[si] & 0xFFFF
                si += 1
                return v

            q = plane + (i * W + j) * 2
            if st.pred1 <= 3 and st.sign != 0:
                # path A: blocks laid out column-major inside each step row
                cnt = W * factor - 1
                k = 0
                while k < factor:
                    q += W * 2
                    n2 = 1
                    while n2 < factor:
                        M.p16(q, gv())
                        q += 4 * W
                        n2 += 2
                    q -= (cnt + W) * 2
                    n3 = 0
                    while n3 < factor:
                        M.p16(q, gv())
                        q += W * 2
                        n3 += 1
                    q -= cnt * 2
                    k += 2
            else:
                # path B: row-major
                cnt = W - factor
                k = 0
                while k < factor:
                    q += 2
                    n2 = 1
                    while n2 < factor:
                        M.p16(q, gv())
                        q += 4
                        n2 += 2
                    q += cnt * 2 - 2
                    n3 = 0
                    while n3 < factor:
                        M.p16(q, gv())
                        q += 2
                        n3 += 1
                    q += cnt * 2
                    k += 2


def _base_grid(M: _Mem, frame: bytes, st: _BitState, plane: int,
               bw: int, bh: int) -> None:
    """Coarse ``(bw/2)x(bh/2)`` byte grid -> the even/even plane samples."""
    lo = _s32(int.from_bytes(frame[st.pos:st.pos + 4], "little"))
    st.pos += 4
    hi = _s32(int.from_bytes(frame[st.pos:st.pos + 4], "little"))
    st.pos += 4
    scale = (hi - lo) / 256.0
    p = st.pos
    idx = 0
    while idx < bh * bw:
        j = 0
        while j < bw:
            v = int(frame[p] * scale + lo)
            p += 1
            M.p16(plane + (idx + j) * 2, v)
            j += 2
        idx += 2 * bw
    st.pos = p


def _decode_planes(frame: bytes, width: int, height: int) -> _Mem:
    """0x49ee8c: reconstruct luma + two chroma planes into a flat buffer."""
    M = _Mem(plane_buffer_size(width, height))
    st = _BitState()
    st.src = frame
    plane = PLANE_BASE
    W4, H4 = width // 4, height // 4
    W8, H8 = width // 8, height // 8
    for pl in range(3):
        st.bc = 0
        if pl == 0:
            _base_grid(M, frame, st, plane, W4, H4)
            _scatter_blocks(M, W4, H4, 4, st, plane)
        else:
            _base_grid(M, frame, st, plane, W8, H8)
            _scatter_blocks(M, W8, H8, 4, st, plane)
            _filt_edges(M, plane, W8, H8)
            _spread(M, plane, W8, H8)
            _scatter_blocks(M, W4, H4, 8, st, plane)
        _filt_edges(M, plane, W4, H4)
        _spread(M, plane, W4, H4)
        _scatter_blocks(M, width // 2, height // 2, 8, st, plane)
        _filt_edges(M, plane, width // 2, height // 2)
        if pl == 0:
            _spread(M, plane, width // 2, height // 2)
            _scatter_blocks(M, width, height, 8, st, plane)
            _filt_edges(M, plane, width, height)
            plane += width * height * 2 + 0x20
        else:
            plane += (width // 2) * (height // 2) * 2 + 0x20
    return M


def decode_frame_planes(frame: bytes, width: int, height: int) -> bytes:
    """Decode one frame block into raw signed-16 plane samples."""
    return bytes(_decode_planes(frame, width, height).m)


def planes_to_rgb(planes: bytes, width: int, height: int) -> bytes:
    """0x49e33b: signed-16 Y/C0/C1 planes -> packed 8-bit RGB."""
    cw, ch = width // 2, height // 2
    lum = memoryview(planes)[:width * height * 2].cast("h")
    o1 = width * height * 2 + 0x20
    ch0 = memoryview(planes)[o1:o1 + cw * ch * 2].cast("h")
    o2 = o1 + cw * ch * 2 + 0x20
    ch1 = memoryview(planes)[o2:o2 + cw * ch * 2].cast("h")
    out = bytearray(width * height * 3)
    o = 0
    for y in range(height):
        lbase = y * width
        cbase = (y >> 1) * cw
        for x in range(width):
            Y = lum[lbase + x]
            c0 = ch0[cbase + (x >> 1)]
            c1 = ch1[cbase + (x >> 1)]
            r = (Y + c0 + 8) >> 4
            g = (Y - c0 - c1 + 8) >> 4
            b = (Y + c1 + 8) >> 4
            out[o] = 0 if r < 0 else (255 if r > 255 else r)
            out[o + 1] = 0 if g < 0 else (255 if g > 255 else g)
            out[o + 2] = 0 if b < 0 else (255 if b > 255 else b)
            o += 3
    return bytes(out)


def decode_frame_rgb(frame: bytes, width: int, height: int) -> bytes:
    """Decode one frame block straight to packed 8-bit RGB."""
    return planes_to_rgb(decode_frame_planes(frame, width, height), width, height)
