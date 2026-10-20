"""Gunmetal ``SOU1`` (4-bit Miles ADPCM) decoder.

Recovered from ``gundx.exe``: three routines ``0x852b40`` / ``0x852c00`` /
``0x852cc0`` are the same loop with 2 / 4 / 8 output slots per sample (the
mixer runs at 32 kHz).  For every payload byte, high nibble first::

    code  = nibble
    delta = curve[base + (level >> 2) + code]      # signed 32-bit dword
    acc   = (acc + delta) & 0xffffffff
    level = (level + step[code]) & 0xffffffff
    emit acc, ``factor`` times (the output buffer is *added* to)

``step`` (``0x850500``) is -64 for codes 0..3 / 8..11 and 128/256/384/512 for
4..7 / 12..15.  ``curve`` is one shared 0x1d80-byte signed-dword table
(``0x850540..0x8522c0``); the 32 pointers at ``0x850480`` select a starting row
by volume (0..31, 31 = loudest).  ``acc`` and ``level`` are per-voice,
per-channel state zeroed when the voice starts (``0x40a5e0``), so a fresh
sample starts at 0/0.

Stereo payloads are nibble-interleaved (loader ``0x40b2d8``): for two bytes
``A B``, the left byte is ``(A & 0xf0) | (B >> 4)`` and the right byte is
``((A << 4) & 0xf0) | (B & 15)``; each half is decoded independently.

The model was verified against the shipped decoder bytes under the x86
emulator on all 983 entries: 67 939 382 slots, 2123.1 s, 0 differences.  The
FNV manifest is ``docs/manifests/sou1-fnv.json`` and the proof ``docs/SOU1.md``.

Unlike SOU2, the 16-bit conversion uses ``acc >> 5`` (the corpus peaks around
2^20), and the WAV is written at the *source* rate by dropping the duplicated
mixer slots.  ``gm/sou.py`` delegates here; the C++ port lives in
``engine/src/res/sou1.*``.
"""

from __future__ import annotations

import struct
from array import array

from ._sou1_tables import CURVE, STEP, VOLUME_BASE

RATE_OUT = 32000
MAX_VOLUME = 31
PCM16_SHIFT = 5
CURVE_BASE = 0x850540  # VA of CURVE[0], for error messages


def factor_for(rate: int) -> int:
    """Output slots per payload byte: 8 / 4 / 2 for 8 / 16 / 32 kHz."""
    if rate < 10000:
        return 8
    if rate < 18000:
        return 4
    return 2


def deinterleave_stereo(payload: bytes) -> tuple[bytes, bytes]:
    """Split a nibble-interleaved SOU1 payload (loader 0x40b2d8)."""
    n = len(payload) & ~1
    left = bytearray(n // 2)
    right = bytearray(n // 2)
    for i in range(0, n, 2):
        a = payload[i]
        b = payload[i + 1]
        left[i // 2] = (a & 0xF0) | (b >> 4)
        right[i // 2] = ((a << 4) & 0xF0) | (b & 0x0F)
    return bytes(left), bytes(right)


def decode_stream(payload: bytes, rate: int, volume: int = MAX_VOLUME) -> array:
    """Decode one mono channel to unsigned 32-bit mixer slots."""
    if not 0 <= volume <= MAX_VOLUME:
        raise ValueError("SOU1 volume must be 0..31")
    copies = factor_for(rate) // 2  # the mixer's per-nibble duplication
    base = VOLUME_BASE[volume]
    max_level = (len(CURVE) - base - 16) * 4
    out = array("I")
    append = out.append
    extend = out.extend
    acc = 0
    level = 0
    for byte in payload:
        if level > max_level:
            raise ValueError(f"SOU1 adaptation level {level:#x} leaves the delta curve")
        w = base + (level >> 2)
        acc = (acc + CURVE[w + (byte >> 4)]) & 0xFFFFFFFF
        append(acc)
        if copies > 1:
            extend([acc] * (copies - 1))
        level = (level + STEP[byte >> 4]) & 0xFFFFFFFF

        if level > max_level:
            raise ValueError(f"SOU1 adaptation level {level:#x} leaves the delta curve")
        w = base + (level >> 2)
        acc = (acc + CURVE[w + (byte & 15)]) & 0xFFFFFFFF
        append(acc)
        if copies > 1:
            extend([acc] * (copies - 1))
        level = (level + STEP[byte & 15]) & 0xFFFFFFFF
    return out


def decode_entry(blob: bytes, volume: int = MAX_VOLUME):
    """Decode one SOU1 blob -> ``(rate, channels, [array('I'), ...])``."""
    if len(blob) < 16 or blob[:4] != b"SOU1":
        raise ValueError(f"not a SOU1 blob (magic {blob[:4]!r})")
    rate, flags = struct.unpack_from("<II", blob, 4)
    length = struct.unpack_from("<I", blob, 12)[0]
    if rate == 0:
        raise ValueError("SOU1 header has sample rate 0")
    payload = blob[16:16 + min(length, len(blob) - 16)]
    payload = payload[:len(payload) & ~1]
    if flags & ~1:
        raise ValueError(f"implausible SOU1 flag word {flags}")
    if flags & 1:
        left, right = deinterleave_stereo(payload)
        streams = [decode_stream(left, rate, volume),
                   decode_stream(right, rate, volume)]
    else:
        streams = [decode_stream(payload, rate, volume)]
    if len({len(s) for s in streams}) != 1:
        raise ValueError("SOU1 channel length mismatch")
    return rate, 1 + (flags & 1), streams


def wav_from_streams(rate: int, channels: int, streams) -> bytes:
    """16-bit WAV at the source rate (the duplicated mixer slots are dropped)."""
    copies = factor_for(rate) // 2
    frames = len(streams[0]) // copies
    pcm = bytearray(frames * channels * 2)
    o = 0
    for i in range(frames):
        for s in streams:
            v = s[i * copies]
            v = v - 0x100000000 if v & 0x80000000 else v
            v >>= PCM16_SHIFT
            v = 32767 if v > 32767 else (-32768 if v < -32768 else v)
            struct.pack_into("<h", pcm, o, v)
            o += 2
    block = channels * 2
    header = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
              + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                      rate * block, block, 16)
              + b"data" + struct.pack("<I", len(pcm)))
    return header + pcm


def to_wav(blob: bytes, volume: int = MAX_VOLUME) -> bytes:
    rate, channels, streams = decode_entry(blob, volume)
    return wav_from_streams(rate, channels, streams)
