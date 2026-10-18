"""Gunmetal ".sou" sound blobs (entries of sounds/SOU.DAT).

The container is fully reversed; the *payload codec* is not.

Container (verified against all 999 entries of sounds/SOU.DAT):

    SOU0 / SOU1                       SOU2
    --------------------------        --------------------------
    char magic[4]   "SOU0"/"SOU1"     char magic[4]   "SOU2"
    u32  sample_rate                  u32  sample_rate
    u32  flags       0 or 1           u32  meta1
    u32  length      payload bytes    u32  meta2
    u8   payload[length]              u32  units     payload bytes = units * 4
                                      u8   payload[units * 4]

For SOU0/SOU1 the payload is ``data[16 : 16 + length]`` (``length`` equals the
remaining file size for every mono entry; the three stereo entries carry
``length == payload + 1``).  For SOU2 the payload is ``data[20:]`` and
``units * 4`` equals the remaining size for all three shipped files.

Codecs — what the game's loader (gundx.exe, function at 0x40af30) does with the
magic digit:

* ``SOU0`` -> format 0: the payload is **raw unsigned 8-bit PCM**.  Only 13
  entries use it (the loops: DRONE, ECMLOOP, FFLOOP, THUMx, F-*, C-PC, MAGDEF).
  These are the only ones this module will turn into a WAV.
* ``SOU1`` -> format 3/2/1 for sample rates below 10000 / 18000 / above: a
  compressed 8-bit-code stream fed to the sound driver (16-bit output).  The
  loader decompresses nothing, so the codec lives in the driver.
* ``SOU2`` -> format 4, the compressed music tracks (FINALE, OVERTURE, WINNER).

The sound system is the Miles Sound System: gundx.exe carries the sections
``adpcmseg``, ``mixasmda``, ``mixasmse``, ``putseg``, ``putsegda``,
``tempscrs`` and the AIL card-probe strings ("SB 16 sound card found", ...).
Those sections are runtime *buffers*; the ADPCM step tables are loaded from AIL
segment files that were not shipped, so the compressed payload cannot be decoded
from the installation alone (see docs/FORMATS.md).  Evidence that the payload is
really coded and not plain PCM: a 0.05 s entry literally named ``SILENCE`` is a
raw 8-bit PCM misinterpretation would be a loud buzz, while its payload is an
exact 3-byte cycle ``4b 0c 20`` (i.e. a delta-codec holding a constant).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGICS = (b"SOU0", b"SOU1", b"SOU2")

# Payload codec families, keyed by the magic digit.
CODEC_PCM = "pcm8"          # SOU0: raw unsigned 8-bit PCM
CODEC_CODED = "coded"       # SOU1 / SOU2: driver side (Miles/AIL) compressed


@dataclass
class Sou:
    magic: str
    codec: str
    sample_rate: int
    flags: int
    payload: bytes
    length_field: int
    meta: tuple[int, ...] = field(default_factory=tuple)

    @property
    def channels(self) -> int:
        """1 for the mono entries; the ``flags == 1`` entries are stereo."""
        return 2 if self.flags == 1 else 1

    @property
    def is_pcm(self) -> bool:
        return self.codec == CODEC_PCM

    def to_wav(self) -> bytes:
        """Unsigned 8-bit PCM WAV.  Only valid for ``is_pcm`` entries."""
        if not self.is_pcm:
            raise ValueError(
                f"{self.magic} payload is a compressed codec, not raw PCM; "
                "refusing to emit a misleading WAV"
            )
        n = len(self.payload)
        fmt = struct.pack(
            "<4sIHHIIHH",
            b"fmt ",
            16,
            1,                     # PCM
            1,                     # mono
            self.sample_rate,
            self.sample_rate,      # byte rate (8-bit mono)
            1,                     # block align
            8,                     # bits
        )
        data = struct.pack("<4sI", b"data", n) + self.payload
        body = b"WAVE" + fmt + data
        return struct.pack("<4sI", b"RIFF", len(body)) + body


def parse(data: bytes) -> Sou:
    if len(data) < 8 or data[:4] not in MAGICS:
        raise ValueError(f"not a SOU blob (magic {data[:4]!r})")
    magic = data[:4].decode("ascii")
    rate = struct.unpack_from("<I", data, 4)[0]

    if magic == "SOU2":
        if len(data) < 20:
            raise ValueError("truncated SOU2 header")
        meta1, meta2, units = struct.unpack_from("<III", data, 8)
        length = units * 4
        payload = data[20 : 20 + length]
        if len(payload) != length:
            raise ValueError(f"truncated SOU2 payload: want {length}, have {len(payload)}")
        return Sou(magic, CODEC_CODED, rate, 0, payload, length, (meta1, meta2))

    if len(data) < 16:
        raise ValueError("truncated SOU0/SOU1 header")
    flags, length = struct.unpack_from("<II", data, 8)
    if flags > 1:
        raise ValueError(f"implausible SOU flag word {flags}")
    # The declared length is exact for mono entries and one greater than the
    # payload for the stereo ones, so size the payload from the file.
    payload = data[16 : 16 + min(length, len(data) - 16)]
    codec = CODEC_PCM if magic == "SOU0" else CODEC_CODED
    return Sou(magic, codec, rate, flags, payload, length)


def parse_file(path: str) -> Sou:
    with open(path, "rb") as f:
        return parse(f.read())


def build_wav(data: bytes) -> bytes:
    return parse(data).to_wav()
