"""Gunmetal ``SOU2`` music codec (engine "format 4").

This is a faithful Python port of ``engine/src/res/sou2.cpp``, which was
recovered from ``gundx.exe`` (decoder ``0x852d80``, mixer ``0x409658``) and
verified against the original code under the x86 emulator on full tracks plus
the first blocks of every archive entry.

Container (all 3092 entries of ``MUSIC/SOU.DAT``)::

    char magic[4]   "SOU2"
    u32  sample_rate     32000 (3090 entries) or 16000 (2)
    u32  frames          stereo frame count
    u32  meta2           not read by the loader
    u32  units           payload = units * 4 bytes
    u8   payload[units * 4]

Every 1024-frame block is split into 13 interleaved sub-streams.  Sub-stream
``c`` owns frame positions ``off + k * esi``::

    esi = 8 << (c >> 2)                              (c == 12: 8)
    off = ((c & 3) << ((c >> 2) + 1)) + (1 << (c >> 2)) - 1   (c == 12: 7)

Each call starts with a unary level delta (and, for levels 1/2, a shift
delta), then a 10-bit selector.  Levels 2 and 3 have a "clean" path (low
nibble 0) and a fractional path (2^-k decay; bit 3 negates the accumulator).
Level 3 uses the step table ``0x8522c0`` and the long, unaligned gain table
``0x852340``; its byte cursor and the bit cursor survive between calls.

The accumulators are 32-bit and wrap like the x86 code, so all arithmetic
goes through :func:`i32`.  ``decode_sou2`` converts them to 16-bit PCM with
``clamp(raw >> 1)``, the same convention as ``tools/re/sou_decode.py`` for
SOU1.

Shipped anomalies: ``L04I01`` has 0 frames; ``L06A01`` declares ``frames=1``
and ``units=0`` but carries 90 592 payload bytes.  The loader allocates
``units * 4`` bytes, so the latter decodes as one silent frame.

CLI: ``gm.py music`` / ``gm.py sounds`` call :func:`to_wav`.
"""

from __future__ import annotations

import struct
from array import array

MAGIC = b"SOU2"
MAX_FRAMES = 20_000_000       # trust-boundary guard; corpus max is 9 711 808
MAX_VOLUME = 32

#: ``0x8522c0``: 16 * -4, then byte offsets 8, 16, ..., 128.
STEP_TABLE = (
    -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4,
    8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 128,
)

#: ``0x852340..0x852ac0``, embedded verbatim (the cursor reaches ~0x6c0).
#: Filled in from ``engine/src/res/sou2.cpp`` by ``tools/gen_sou2_table.py``.
GAIN_TABLE = bytes.fromhex(
    "25000000260000002700000028000000290000002a0000002b0000002c000000"
    "2d0000002e0000002f0000003000000031000000330000003400000035000000"
    "3600000038000000390000003a0000003c0000003d0000003f00000040000000"
    "42000000430000004500000047000000480000004a0000004c0000004e000000"
    "4f000000510000005300000055000000570000005a0000005c0000005e000000"
    "600000006200000065000000670000006a0000006c0000006f00000072000000"
    "74000000770000007a0000007d00000080000000830000008600000089000000"
    "8d0000009000000094000000970000009b0000009f000000a2000000a6000000"
    "aa000000ae000000b3000000b7000000bb000000c0000000c5000000c9000000"
    "ce000000d3000000d8000000dd000000e3000000e8000000ee000000f4000000"
    "f9000000ff000000060100000c01000012010000190100002001000027010000"
    "2e010000350100003c010000440100004c010000540100005c01000065010000"
    "6d010000760100007f01000088010000920100009b010000a5010000af010000"
    "ba010000c4010000cf010000db010000e6010000f2010000fe0100000a020000"
    "1702000023020000310200003e0200004c0200005a0200006902000078020000"
    "8702000096020000a6020000b7020000c8020000d9020000ea020000fc020000"
    "0f0300002203000035030000490300005d03000072030000870300009d030000"
    "b3030000ca030000e1030000f9030000120400002b040000450400005f040000"
    "7a04000096040000b2040000cf040000ec0400000b0500002a0500004a050000"
    "6b0500008c050000ae050000d1050000f50500001a0600004006000066060000"
    "8e060000b6060000e00600000a070000350700006207000090070000be070000"
    "ee0700001f0800005108000084080000b9080000ef080000260900005e090000"
    "98090000d4090000100a00004e0a00008e0a0000cf0a0000120b0000560b0000"
    "9c0b0000e40b00002d0c0000780c0000c50c0000140d0000650d0000b80d0000"
    "0c0e0000630e0000bc0e0000170f0000740f0000d30f00003510000099100000"
    "0011000068110000d411000042120000b3120000261300009c13000015140000"
    "91140000101500009215000018160000a01600002c170000bb1700004d180000"
    "e31800007d1900001a1a0000bb1a0000601b0000091c0000b61c0000671d0000"
    "1d1e0000d71e0000951f00005820000020210000ec210000be22000094230000"
    "70240000512500003726000023270000152800000c290000092a00000d2b0000"
    "172c0000272d00003e2e00005b2f00007f300000ab310000dd32000017340000"
    "59350000a2360000f43700004d390000af3a0000193c00008c3d0000083f0000"
    "8d4000001b420000b34300005545000001470000b8480000794a0000444c0000"
    "1b4e0000fd4f0000eb510000e5530000eb550000fd5700001c5a0000485c0000"
    "825e0000ca6000001f63000083650000f6670000776a0000086d0000aa6f0000"
    "5b7200001d750000f0770000d47a0000ca7d0000d3800000ee8300001d870000"
    "5f8a0000b58d0000209100009f94000035980000e09b0000a39f00007ca30000"
    "6da7000077ab000099af0000d5b300002bb800009cbc000029c10000d1c50000"
    "96ca000079cf00007ad4000099d90000d8de000038e40000b9e900005cef0000"
    "21f500000afb0000180101004b070100a50d010025140100ce1a0100a0210100"
    "9b280100c22f010016370100963e010045460100234e010031560100725e0100"
    "e56601008d6f0100697801007d810100c98a01004e9401000e9e01000aa80100"
    "43b20100bcbc010075c7010071d20100b0dd010035e9010001f5010016010200"
    "750d0200211a02001b2702006534020001420200f14f0200375e0200d56c0200"
    "ce7b0200238b0200d69a0200ebaa020062bb020040cc020085dd020035ef0200"
    "53010300e0130300df260300543a0300414e0300a96203008f770300f68c0300"
    "e1a2030054b9030051d00300dce70300f8ff0300a9180400f3310400d84b0400"
    "5e66040087810400589d0400d4b9040001d70400e1f404007a130500d0320500"
    "e7520500c47305006d950500e5b7050031db050058ff05005e240600484a0600"
    "1d710600e19806009ac106004feb060006160700c4410700906e0700719c0700"
    "6dcb07008bfb0700d22c0800495f0800f8920800e6c708001afe08009d350900"
    "776e0900b0a8090050e4090060210a00e95f0a00f49f0a008be10a00b7240b00"
    "81690b00f3af0b0019f80b00fc410c00a78d0c0025db0c00822a0d00c87b0d00"
    "05cf0d0043240e008f7b0e00f6d40e0085300f00498e0f0050ee0f00a8501000"
    "5fb51000841c11002586110053f211001c61120091d21200c2461300c1bd1300"
    "9f3714006cb414003c34150021b715002e3d160077c616000f5317000be31700"
    "80761800830d19002aa819008c461a00c0e81a00dd8e1b00fb381c0034e71c00"
    "a0991d0059501e007b0b1f001fcb1f0000000000000000000000000000000000"
    "5351525657558b44241c8b5424208b5c24248b4c242889c6a0a8a6840089dd83"
    "e01fd1ed7477518b198b49048b3c85800485008a06893de32a8500893dff2a85"
)


def i32(v: int) -> int:
    """Truncate to a signed 32-bit integer (x86 wrap-around)."""
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def add(a: int, b: int) -> int:
    return i32(a + b)


def sub(a: int, b: int) -> int:
    return i32(a - b)


def mul(a: int, b: int) -> int:
    return i32(a * b)


def neg(a: int) -> int:
    return i32(-a)


def sar(v: int, n: int) -> int:
    return v >> (n & 31)


def int6(peek: int) -> int:
    x = (peek >> 4) & 0x3F
    return x - 0x40 if x & 0x20 else x


def _positions(rem: int, esi: int) -> int:
    return (rem + esi - 1) // esi


class _BitReader:
    """Shared bit cursor (engine globals 0x84e31c/0x84e320/0x84e324)."""

    __slots__ = ("data", "size", "ptr", "acc", "bits", "overrun", "bad_cursor")

    def __init__(self, data: bytes):
        self.data = data
        self.size = len(data)
        self.ptr = 0
        self.acc = 0
        self.bits = 0
        self.overrun = False
        self.bad_cursor = False

    def bit(self) -> int:
        bits = self.bits - 1
        self.bits = bits
        if bits < 0:
            ptr = self.ptr
            if ptr + 4 > self.size:
                self.overrun = True
                self.bits = 0
                return 0
            self.acc = struct.unpack_from("<I", self.data, ptr)[0]
            self.ptr = ptr + 4
            self.bits = 31
        acc = self.acc
        b = 1 if acc & 0x80000000 else 0
        self.acc = (acc << 1) & 0xFFFFFFFF
        return b

    def unary(self) -> int:
        n = 0
        while not self.bit():
            if self.overrun:
                return n
            n += 1
        return n

    def peek(self, count: int) -> int:
        v = 0
        for _ in range(count):
            v = ((v << 1) | self.bit()) & 0xFFFFFFFF
            if self.overrun:
                return v
        return v


class _Slot:
    __slots__ = ("shift", "level", "prev", "cursor")

    def __init__(self):
        self.shift = 0
        self.level = 0
        self.prev = 0
        self.cursor = 0


def _fill_silence(left, right, rem: int, esi: int, off: int, slot: _Slot) -> None:
    n = _positions(rem, esi)
    for k in range(n):
        p = off + k * esi
        left[p] = 0
        right[p] = 0
    slot.prev = 0


def _level2_clean(br, slot, rem, esi, off, s, peek, left, right) -> bool:
    gain = int6(peek)
    step_l = sar(add(s, 0x10), 5)
    step_r = sar(add(mul(gain, s), 0x200), 10)
    n = _positions(rem, esi)
    last = 0
    for k in range(n):
        p = off + k * esi
        if br.bit():
            left[p] = 0
            right[p] = 0
            last = 0
            continue
        mask = -br.bit()
        a = 0
        b = 0
        while True:
            a = add(a, step_l)
            b = add(b, step_r)
            if br.overrun:
                return False
            if br.bit():
                break
        a ^= mask
        b ^= mask
        left[p] = a
        right[p] = b
        last = a
    slot.prev = last
    return True


def _level2_frac(br, slot, rem, esi, off, s, peek, left, right) -> bool:
    gain = int6(peek)
    step_l = sar(add(s, 0x10), 5)
    step_r = sar(add(mul(gain, s), 0x200), 10)
    negate = bool(peek & 8)
    k = sub(peek & 7, 0 if negate else 1)
    a = slot.prev
    b = sar(add(mul(gain, a), 0x10), 5)
    n = _positions(rem, esi)
    for i in range(n):
        p = off + i * esi
        if negate:
            a = neg(a)
            b = neg(b)
        a = sar(a, k)
        b = sar(b, k)
        if br.bit():
            left[p] = a
            right[p] = b
            continue
        mask = -br.bit()
        a ^= mask
        b ^= mask
        while True:
            a = add(a, step_l)
            b = add(b, step_r)
            if br.overrun:
                return False
            if br.bit():
                break
        a ^= mask
        b ^= mask
        left[p] = a
        right[p] = b
    slot.prev = a
    return True


def _level3_step(br, slot, volume: int) -> int:
    code = br.peek(6)
    sign = -1 if code < 0x20 else 0
    code &= 0x1F
    cursor = slot.cursor
    if cursor < 0 or cursor + 4 > len(GAIN_TABLE):
        br.bad_cursor = True
        return 0
    sign ^= i32(struct.unpack_from("<I", GAIN_TABLE, cursor)[0])
    slot.cursor = add(cursor, STEP_TABLE[code])
    step = sar(mul(sign, 2 * code + 1), 8)
    return sar(add(mul(step, volume), 0x10), 5)


def _level3_clean(br, slot, rem, esi, off, volume, peek, left, right) -> bool:
    gain = int6(peek)
    n = _positions(rem, esi)
    last = 0
    for k in range(n):
        p = off + k * esi
        step = _level3_step(br, slot, volume)
        if br.bad_cursor:
            return False
        left[p] = step
        right[p] = sar(mul(step, gain), 5)
        last = step
    slot.prev = last
    return True


def _level3_frac(br, slot, rem, esi, off, volume, peek, left, right) -> bool:
    gain = int6(peek)
    negate = bool(peek & 8)
    k = sub(peek & 7, 0 if negate else 1)
    a = slot.prev
    n = _positions(rem, esi)
    for i in range(n):
        p = off + i * esi
        if negate:
            a = neg(a)
        a = sar(a, k)
        step = _level3_step(br, slot, volume)
        if br.bad_cursor:
            return False
        a = add(a, step)
        left[p] = a
        right[p] = sar(mul(a, gain), 5)
    slot.prev = a
    return True


def _decode_call(br, slot, rem, esi, off, volume, left, right) -> bool:
    save_ptr, save_acc, save_bits = br.ptr, br.acc, br.bits

    level = br.unary()
    if level != 0 and br.bit():
        level = -level
    slot.level = add(slot.level, level)
    level = slot.level

    if level == 0:
        _fill_silence(left, right, rem, esi, off, slot)
        return not br.overrun and not br.bad_cursor

    if level == 3:
        peek = br.peek(10)
        if (peek & 0xF) == 0:
            ok = _level3_clean(br, slot, rem, esi, off, volume, peek, left, right)
        else:
            ok = _level3_frac(br, slot, rem, esi, off, volume, peek, left, right)
        return ok and not br.overrun and not br.bad_cursor

    shift = br.unary()
    if shift != 0 and br.bit():
        shift = -shift
    slot.shift = add(slot.shift, shift)
    s = i32(volume << (slot.shift & 31))
    peek = br.peek(10)

    if level != 2:
        # 0x852f36: give up on this sub-stream; level/shift writes persist,
        # but the shared cursor and samples do not.
        br.ptr, br.acc, br.bits = save_ptr, save_acc, save_bits
        br.overrun = False
        return True

    if (peek & 0xF) == 0:
        ok = _level2_clean(br, slot, rem, esi, off, s, peek, left, right)
    else:
        ok = _level2_frac(br, slot, rem, esi, off, s, peek, left, right)
    return ok and not br.overrun and not br.bad_cursor


def parse_header(blob: bytes) -> tuple[int, int, int]:
    """Return ``(sample_rate, frames, units)``; raises ``ValueError``."""
    if len(blob) < 20:
        raise ValueError("truncated SOU2 header")
    if blob[:4] != MAGIC:
        raise ValueError(f"not a SOU2 blob (magic {blob[:4]!r})")
    rate, frames, _meta2, units = struct.unpack_from("<IIII", blob, 4)
    if rate == 0:
        raise ValueError("SOU2 header has sample rate 0")
    if units > (len(blob) - 20) // 4:
        raise ValueError("truncated SOU2 payload")
    if frames > MAX_FRAMES:
        raise ValueError(f"implausible SOU2 frame count {frames}")
    return rate, frames, units


def decode_sou2_raw(blob: bytes, volume: int = MAX_VOLUME) -> tuple[int, array]:
    """Decode *blob* into ``(sample_rate, array('i') of interleaved L/R)``."""
    rate, frames, units = parse_header(blob)
    if not 0 <= volume <= MAX_VOLUME:
        raise ValueError("SOU2 volume must be 0..32")

    samples = array("i", bytes(8 * frames))  # int32 zeroed, 4 bytes each
    payload_size = units * 4
    if payload_size == 0 or frames == 0:
        return rate, samples

    br = _BitReader(blob[20:20 + payload_size])
    slots = [_Slot() for _ in range(13)]
    left = [0] * 1024
    right = [0] * 1024
    done = 0
    while done < frames:
        rem = min(1024, frames - done)
        for i in range(rem):
            left[i] = 0
            right[i] = 0
        for c, (esi, off) in enumerate(SUBSTREAM_TABLE):
            if not _decode_call(br, slots[c], rem, esi, off, volume, left, right):
                if br.bad_cursor:
                    raise ValueError("SOU2 level-3 gain table cursor out of range")
                raise ValueError(f"SOU2 bit stream overrun at byte {br.ptr} "
                                 f"of {payload_size}")
        o = done * 2
        for f in range(rem):
            samples[o] = left[f]
            samples[o + 1] = right[f]
            o += 2
        done += rem
    return rate, samples


def decode_sou2(blob: bytes, volume: int = MAX_VOLUME) -> tuple[int, bytes]:
    """Decode to little-endian interleaved 16-bit stereo PCM."""
    rate, raw = decode_sou2_raw(blob, volume)
    out = array("h", bytes(2 * len(raw)))
    for i, v in enumerate(raw):
        v >>= 1
        out[i] = 32767 if v > 32767 else (-32768 if v < -32768 else v)
    return rate, out.tobytes()


def to_wav(blob: bytes, volume: int = MAX_VOLUME) -> bytes:
    """Decode *blob* and wrap the PCM in a canonical 44-byte WAV header."""
    rate, pcm = decode_sou2(blob, volume)
    channels = 2
    bits = 16
    block = channels * bits // 8
    header = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
              + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                      rate * block, block, bits)
              + b"data" + struct.pack("<I", len(pcm)))
    return header + pcm


#: ``{esi, off}`` per sub-stream, in engine order (c == 12 is the odd one).
SUBSTREAM_TABLE = (
    (8, 0), (8, 2), (8, 4), (8, 6),
    (16, 1), (16, 5), (16, 9), (16, 13),
    (32, 3), (32, 11), (32, 19), (32, 27),
    (8, 7),
)
