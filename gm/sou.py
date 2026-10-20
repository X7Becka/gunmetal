"""Gunmetal ``.sou`` sound blobs (entries of sounds/SOU.DAT / MUSIC/SOU.DAT).

Container only — the payload codecs live in :mod:`gm.sou1` and :mod:`gm.sou2`.

Layout (verified against loader ``0x40af30`` in ``gundx.exe`` and all
999 entries of ``sounds/SOU.DAT`` plus all 3092 of ``MUSIC/SOU.DAT``)::

    SOU0 / SOU1                       SOU2
    --------------------------        --------------------------
    char magic[4]   "SOU0"/"SOU1"     char magic[4]   "SOU2"
    u32  sample_rate                  u32  sample_rate
    u32  flags       0 or 1           u32  frames     > 0, multiple of 32
    u32  length      payload bytes    u32  meta2      stored, not read by game
    u8   payload[length & ~1]         u32  units      payload bytes = units * 4
                                      u8   payload[units * 4]

The loader reads the first four bytes and checks ``"SOU"`` plus a digit
``0/1/2``.  For SOU0/SOU1 it then reads rate, flags and length, clears the low
bit of ``length`` (``and al,0xfe`` at ``0x40b0dd``) and reads exactly that many
payload bytes.  ``flags`` is compared as a byte against 1 (``0x40b051``) and
only bit 0 is used: bit 0 means stereo.  The shipped corpus has 6 stereo
entries; three of them (``CUT-EMP``, ``VRCHAMBR``, ``VRCHAMHQ``) declare an odd
``length`` that is one greater than the file payload, which is exactly the
case the loader's ``length & ~1`` handles.

For SOU2 the loader reads rate and the field at offset 8, rejects the file
unless that field is positive and a multiple of 32, reads (but does not use)
the field at offset 12, then ``units`` at offset 16, allocates ``units * 4``
bytes and reads exactly that payload.  Two shipped music entries are rejected
by this loader: ``L04I01`` (``frames == 0``, ``units == 0``) and ``L06A01``
(``frames == 1``, ``units == 0`` plus a 90 592-byte tail).  They are still
representable and round-trip byte-exactly: :func:`parse` keeps the tail in
:attr:`Sou.trailer` and reports the anomalies through :func:`stats`.

Codecs — what the game's loader (VA ``0x40af30``) does with the magic digit:

* ``SOU0`` -> format 0: the payload is **raw unsigned 8-bit PCM**.  Only 13
  entries use it (the loops: DRONE, ECMLOOP, FFLOOP, THUMx, F-*, C-PC, MAGDEF).
* ``SOU1`` -> format 3/2/1 for sample rates below 10000 / 18000 / above: a
  4-bit ADPCM stream (see :mod:`gm.sou1`).
* ``SOU2`` -> format 4, the compressed music tracks (see :mod:`gm.sou2`).

There is **no writer in the shipped binary**.  The only reference to the
``"SOU"`` base string (VA ``0x4a689c``) is the loader wrapper at ``0x40d1a0``;
the three call sites of ``0x40af30`` are all the ``load sample`` path
(``0x40ae1e``, ``0x40ae51``, ``0x40af00``), and no code builds ``SOU0/1/2``
header bytes.  Sound data therefore has to be read byte-exactly; rebuilding is
a tool-side operation, documented in ``docs/SOU_CONTAINER.md``.

The container is fully understood; the payload codecs are not needed for
``stats`` or ``rebuild``.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterable

MAGICS = (b"SOU0", b"SOU1", b"SOU2")

# Payload codec families, keyed by the magic digit.
CODEC_PCM = "pcm8"          # SOU0: raw unsigned 8-bit PCM
CODEC_ADPCM = "adpcm4"      # SOU1: 4-bit Miles ADPCM
CODEC_SOU2 = "sou2"         # SOU2: format 4

# Trust-boundary limits.  Shipped blobs: at most 3 126 040 bytes, rate 32000,
# SOU2 frames up to 9 711 808.  These are guards against hostile archives, not
# format limits.
MAX_PAYLOAD = 64 * 1024 * 1024
MAX_SOU2_UNITS = MAX_PAYLOAD // 4
MIN_RATE = 10                    # loader: cmp rate,0xa / jle reject
MAX_RATE = 1_000_000


@dataclass
class Sou:
    magic: str
    codec: str
    sample_rate: int
    flags: int
    payload: bytes
    length_field: int
    meta: tuple[int, ...] = field(default_factory=tuple)
    raw: bytes = b""          # original blob, needed by the SOU2 decoder
    trailer: bytes = b""      # bytes after the declared payload (L06A01)

    @property
    def channels(self) -> int:
        """2 for SOU2 and the ``flags & 1`` entries, otherwise 1."""
        if self.magic == "SOU2":
            return 2
        return 2 if self.flags & 1 else 1

    @property
    def is_pcm(self) -> bool:
        return self.codec == CODEC_PCM

    @property
    def frames(self) -> int:
        """Sample frames per channel (declared for SOU2)."""
        if self.magic == "SOU2":
            return self.meta[0] if self.meta else 0
        if self.magic == "SOU1":
            # Payload bytes are nibble pairs: two samples per byte per channel.
            return 2 * len(self.payload) // self.channels
        # SOU0: one unsigned 8-bit sample per payload byte per channel.
        return len(self.payload) // self.channels

    @property
    def seconds(self) -> float:
        if self.sample_rate <= 0:
            return 0.0
        return self.frames / self.sample_rate

    @property
    def loadable(self) -> bool:
        """Whether ``0x40af30`` would accept the header (not the codec)."""
        if not MIN_RATE < self.sample_rate <= MAX_RATE:
            return False
        if self.magic == "SOU2":
            frames, _meta2 = (self.meta + (0, 0))[:2]
            units = len(self.payload) // 4
            return (frames > 0 and frames % 32 == 0 and units > 0
                    and len(self.payload) == units * 4)
        return (self.flags & ~1) == 0 and len(self.payload) > 0

    @property
    def ignored_bytes(self) -> int:
        """Payload-area bytes the loader does not feed to the codec.

        For SOU0/SOU1 this is the dropped last byte of an odd ``length`` plus
        any bytes past ``length``; for SOU2 it is the tail after ``units * 4``.
        """
        return len(self.trailer)

    @property
    def issues(self) -> list[str]:
        """Non-fatal oddities (the blob still parses and rebuilds)."""
        out: list[str] = []
        if not self.loadable:
            out.append("loader would reject the header")
        if self.magic != "SOU2":
            if self.length_field & 1:
                out.append("odd length field (last byte not read)")
            elif self.trailer:
                out.append(f"{len(self.trailer)} trailing bytes")
        elif self.trailer:
            out.append(f"{len(self.trailer)} trailing bytes after units*4")
        if self.magic == "SOU2":
            frames, _meta2 = (self.meta + (0, 0))[:2]
            if frames == 0:
                out.append("frames == 0")
            elif frames % 32:
                out.append(f"frames {frames} not a multiple of 32")
            if not self.payload:
                out.append("units == 0")
        return out

    def to_wav(self) -> bytes:
        """WAV for raw PCM (SOU0) or the decoded format-4 stream (SOU2)."""
        if self.magic == "SOU2":
            from . import sou2
            return sou2.to_wav(self.raw)
        if self.magic == "SOU1":
            from . import sou1
            return sou1.to_wav(self.raw)
        if not self.is_pcm:
            raise ValueError(
                f"{self.magic} payload is a compressed codec, not raw PCM; "
                "refusing to emit a misleading WAV"
            )
        n = len(self.payload)
        channels = self.channels
        fmt = struct.pack(
            "<4sIHHIIHH",
            b"fmt ",
            16,
            1,                     # PCM
            channels,
            self.sample_rate,
            self.sample_rate * channels,   # byte rate (8-bit samples)
            channels,                      # block align
            8,                             # bits
        )
        data = struct.pack("<4sI", b"data", n) + self.payload
        body = b"WAVE" + fmt + data
        return struct.pack("<4sI", b"RIFF", len(body)) + body

    # -- writer ---------------------------------------------------------
    def _encode(self) -> bytes:
        """Serialize the current fields without touching the payload bytes."""
        if self.magic == "SOU2":
            frames, meta2 = (self.meta + (0, 0))[:2]
            if len(self.payload) % 4:
                raise ValueError("SOU2 payload must be a multiple of 4 bytes")
            units = len(self.payload) // 4
            header = struct.pack("<4sIIII", b"SOU2", self.sample_rate,
                                 frames, meta2, units)
            return header + self.payload + self.trailer

        length = self.length_field
        if (length & ~1) != len(self.payload):
            # Payload was edited: write a canonical (even) length.
            length = len(self.payload)
        header = struct.pack("<4sIII", self.magic.encode("ascii"),
                             self.sample_rate, self.flags, length)
        return header + self.payload + self.trailer

    def to_bytes(self) -> bytes:
        """Structural rebuild; shipped containers come back byte-exactly."""
        return self._encode()

    def rebuild(self) -> bytes:
        """``to_bytes()`` with an explicit raw round-trip check.

        The shipped files are never normalised: all 481 odd ``length`` fields
        (478 of which make the loader drop one byte) and the ``L06A01`` SOU2
        tail are preserved.
        """
        data = self._encode()
        if self.raw and data != self.raw:
            raise ValueError(
                f"{self.magic} rebuild is not byte-exact: "
                f"{len(data)} != {len(self.raw)} bytes")
        return data


def parse(data: bytes) -> Sou:
    if len(data) < 8 or data[:4] not in MAGICS:
        raise ValueError(f"not a SOU blob (magic {data[:4]!r})")
    magic = data[:4].decode("ascii")
    rate = struct.unpack_from("<I", data, 4)[0]
    if rate == 0:
        raise ValueError(f"{magic}: sample rate is 0")

    if magic == "SOU2":
        if len(data) < 20:
            raise ValueError("truncated SOU2 header")
        frames, meta2, units = struct.unpack_from("<III", data, 8)
        if units > MAX_SOU2_UNITS:
            raise ValueError(f"SOU2 units {units} exceeds limit "
                             f"{MAX_SOU2_UNITS}")
        length = units * 4
        if len(data) - 20 < length:
            raise ValueError(f"truncated SOU2 payload: want {length}, "
                             f"have {len(data) - 20}")
        payload = data[20 : 20 + length]
        trailer = data[20 + length :]
        return Sou(magic, CODEC_SOU2, rate, 0, payload, length,
                   (frames, meta2), data, trailer)

    if len(data) < 16:
        raise ValueError("truncated SOU0/SOU1 header")
    flags, length = struct.unpack_from("<II", data, 8)
    if flags > 1:
        raise ValueError(f"implausible SOU flag word {flags}")
    if length > MAX_PAYLOAD:
        raise ValueError(f"{magic}: length {length} exceeds limit "
                         f"{MAX_PAYLOAD}")
    # 0x40b0dd: the loader clears bit 0 before allocating/reading.
    effective = length & ~1
    available = len(data) - 16
    if effective > available:
        raise ValueError(f"truncated {magic} payload: want {effective}, "
                         f"have {available}")
    payload = data[16 : 16 + effective]
    trailer = data[16 + effective :]
    codec = CODEC_PCM if magic == "SOU0" else CODEC_ADPCM
    return Sou(magic, codec, rate, flags, payload, length, (), data, trailer)


def parse_file(path: str) -> Sou:
    with open(path, "rb") as f:
        return parse(f.read())


def build_wav(data: bytes) -> bytes:
    return parse(data).to_wav()


# ---------------------------------------------------------------------------
# statistics
#
# ``stats`` is deliberately codec-free: it never imports gm/sou1 or gm/sou2,
# so it stays fast on the whole archive (the duration formulas are the ones
# documented above and in the codec modules).
# ---------------------------------------------------------------------------

def blob_info(data: bytes, name: str | None = None) -> dict:
    """One-entry record used by :func:`stats`; bad blobs are recorded too."""
    record: dict = {"name": name, "bytes": len(data)}
    try:
        snd = parse(data)
    except ValueError as exc:
        record.update(ok=False, error=str(exc))
        return record
    record.update(
        ok=True,
        magic=snd.magic,
        codec=snd.codec,
        rate=snd.sample_rate,
        channels=snd.channels,
        flags=snd.flags,
        payload=len(snd.payload),
        frames=snd.frames,
        seconds=round(snd.seconds, 6),
        trailer=len(snd.trailer),
        ignored=snd.ignored_bytes,
        loadable=snd.loadable,
        issues=snd.issues,
    )
    return record


def _as_items(entries: Iterable) -> Iterable[tuple[str | None, bytes]]:
    for item in entries:
        if isinstance(item, tuple):
            name, data = item
            yield name, data
        else:
            yield None, item


def stats(entries: Iterable) -> dict:
    """Aggregate container stats over ``bytes`` or ``(name, bytes)`` items.

    Returns a JSON-friendly dict with format/rate/channel counts, payload and
    duration totals, and the list of rejected blobs.  No payload is decoded.
    """
    out: dict = {
        "entries": 0,
        "parsed": 0,
        "bad": 0,
        "bytes": 0,
        "payload_bytes": 0,
        "frames": 0,
        "seconds": 0.0,
        "formats": {},
        "rates": {},
        "channels": {},
        "loadable": 0,
        "ignored_bytes": 0,
        "with_trailer": 0,
        "anomalies": {},
        "errors": [],
    }
    for name, data in _as_items(entries):
        out["entries"] += 1
        out["bytes"] += len(data)
        info = blob_info(data, name)
        if not info["ok"]:
            out["bad"] += 1
            if len(out["errors"]) < 50:
                out["errors"].append({"name": name, "error": info["error"]})
            continue
        out["parsed"] += 1
        out["payload_bytes"] += info["payload"]
        out["frames"] += info["frames"]
        out["seconds"] += info["seconds"]
        key = info["magic"]
        out["formats"][key] = out["formats"].get(key, 0) + 1
        rkey = str(info["rate"])
        out["rates"][rkey] = out["rates"].get(rkey, 0) + 1
        ckey = str(info["channels"])
        out["channels"][ckey] = out["channels"].get(ckey, 0) + 1
        if info["loadable"]:
            out["loadable"] += 1
        out["ignored_bytes"] += info["ignored"]
        if info["ignored"]:
            out["with_trailer"] += 1
        for issue in info["issues"]:
            out["anomalies"][issue] = out["anomalies"].get(issue, 0) + 1
    out["seconds"] = round(out["seconds"], 3)
    out["formats"] = dict(sorted(out["formats"].items()))
    out["rates"] = dict(sorted(out["rates"].items(), key=lambda kv: int(kv[0])))
    out["channels"] = dict(sorted(out["channels"].items()))
    out["anomalies"] = dict(sorted(out["anomalies"].items()))
    return out
